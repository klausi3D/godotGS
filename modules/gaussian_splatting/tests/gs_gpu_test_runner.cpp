/**************************************************************************/
/*  gs_gpu_test_runner.cpp                                                */
/*  Phase 2 of the Gaussian Splatting GPU test harness. Builds on the     */
/*  Phase 1 spike (offscreen RenderingDevice via Main::test_setup() plus  */
/*  RenderingContextDriverVulkan) by integrating doctest::Context so the  */
/*  existing [RequiresGPU] TEST_CASEs actually execute instead of         */
/*  skipping. Argv pass-through mirrors tests/test_main.cpp:266-300.      */
/**************************************************************************/

#ifdef TESTS_ENABLED

#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/object/worker_thread_pool.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "main/main.h"

#include "modules/gaussian_splatting/core/gaussian_splat_scene_director.h"

#include "tests/display_server_mock.h"
#include "tests/test_macros.h"

#if defined(RD_ENABLED)
#include "servers/rendering/rendering_device.h"
#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_context_driver_d3d12.h"
#endif
#endif

#include <cstring>

namespace {

// Exit codes — stable contract for run_gpu_harness.py to interpret.
constexpr int GS_GPU_EXIT_FAIL_TEST_SETUP = 70;
constexpr int GS_GPU_EXIT_NO_DRIVER = 64;
constexpr int GS_GPU_EXIT_DRIVER_INIT_FAILED = 65;
constexpr int GS_GPU_EXIT_RD_INIT_FAILED = 66;
constexpr int GS_GPU_EXIT_RD_NOT_COMPILED = 71;

#if defined(RD_ENABLED)
static RenderingContextDriver *_rcd = nullptr;
static RenderingDevice *_rd = nullptr;
static const char *_driver_label = "none";
#endif

struct GsGpuTestOptions {
	String preferred_driver;
	bool inject_default_filter = true;
};

GsGpuTestOptions _parse_options(int argc, char *argv[]) {
	GsGpuTestOptions opt;
	for (int x = 0; x < argc; x++) {
		const char *a = argv[x];
		if (strncmp(a, "--gs-gpu-driver=", 16) == 0) {
			opt.preferred_driver = String(a + 16);
		} else if (strncmp(a, "--test-case=", 12) == 0) {
			// Only an INCLUSIVE filter suppresses the default-filter injection.
			// `--test-case-exclude=` alone should still get the default
			// `*[RequiresGPU]*` filter; otherwise doctest would run every test
			// in the binary, including non-GPU cases this bootstrap cannot serve.
			opt.inject_default_filter = false;
		}
	}
	return opt;
}

#if defined(RD_ENABLED)
int _bootstrap_rd(const GsGpuTestOptions &opt) {
	const String pref = opt.preferred_driver.to_lower().strip_edges();

#if defined(VULKAN_ENABLED)
	if (pref.is_empty() || pref == "vulkan") {
		_rcd = memnew(RenderingContextDriverVulkan);
		_driver_label = "vulkan";
	}
#endif
#ifdef D3D12_ENABLED
	if (_rcd == nullptr && (pref.is_empty() || pref == "d3d12")) {
		_rcd = memnew(RenderingContextDriverD3D12);
		_driver_label = "d3d12";
	}
#endif

	if (_rcd == nullptr) {
		print_error(vformat("[GS-GPU] FAIL: no rendering context driver compiled for preference '%s'",
				pref.is_empty() ? String("auto") : pref));
		return GS_GPU_EXIT_NO_DRIVER;
	}

	Error err = _rcd->initialize();
	if (err != OK) {
		print_error(vformat("[GS-GPU] FAIL: %s rcd->initialize() returned %d", _driver_label, int(err)));
		memdelete(_rcd);
		_rcd = nullptr;
		return GS_GPU_EXIT_DRIVER_INIT_FAILED;
	}

	_rd = memnew(RenderingDevice);
	err = _rd->initialize(_rcd);
	if (err != OK) {
		print_error(vformat("[GS-GPU] FAIL: %s rd->initialize() returned %d", _driver_label, int(err)));
		memdelete(_rd);
		_rd = nullptr;
		memdelete(_rcd);
		_rcd = nullptr;
		return GS_GPU_EXIT_RD_INIT_FAILED;
	}

	print_line(vformat("[GS-GPU] driver=%s adapter=\"%s\" vendor=\"%s\"",
			_driver_label,
			_rd->get_device_name(),
			_rd->get_device_vendor_name()));
	return 0;
}

void _teardown_rd() {
	if (_rd) {
		memdelete(_rd);
		_rd = nullptr;
	}
	if (_rcd) {
		memdelete(_rcd);
		_rcd = nullptr;
	}
}
#endif // RD_ENABLED

int _run_doctest(int argc, char *argv[], const GsGpuTestOptions &opt) {
	doctest::Context ctx;
	LocalVector<String> test_args;

	for (int x = 0; x < argc; x++) {
		String arg = String(argv[x]);
		if (arg == "--gs-gpu-test") {
			continue;
		}
		if (arg.begins_with("--gs-gpu-driver=")) {
			continue;
		}
		test_args.push_back(arg);
	}

	if (opt.inject_default_filter) {
		test_args.push_back(String("--test-case=*[RequiresGPU]*"));
		// Also exclude tag categories that share `[RequiresGPU]` but need
		// fuller engine bootstrap than the harness provides. Without these
		// exclusions, plain `--gs-gpu-test` (no filter) hangs or crashes
		// on the first such case it encounters — making the default
		// invocation unusable for local debugging or any caller that
		// forgets to pass a narrower filter.
		//
		// Excluded categories:
		//   - `[SceneTree]`: these DO run under this harness now (#329 registers
		//     the mock DisplayServer driver below and fixes teardown order), but a
		//     handful still crash hard -- one of them takes the whole process down
		//     via a non-aborting REQUIRE (#656). A bare `--gs-gpu-test` with no
		//     filter is a convenience entry point, so it stays conservative. The
		//     supervisor runs them properly through the NodeSceneTree /
		//     WorldSceneTree / SceneDirectorSceneTree batches, which pass explicit
		//     `--test-case=` filters and therefore bypass this injection entirely.
		//     The still-broken cases are listed as BatchSpec.excludes there, each
		//     with a waiver in the release-gate manifest.
		//   - `[Importer]`: needs full ResourceLoader/ResourceSaver setup
		//     including type-registration for GaussianSplatAsset's preview
		//     I/O paths; the offscreen RD bootstrap is intentionally
		//     lighter weight and crashes inside resource roundtrip.
		//
		// Callers who genuinely want to include any of these must pass an
		// explicit `--test-case=` (which sets inject_default_filter=false
		// and disables this injection block entirely). The supervisor uses
		// explicit per-batch filters, so it bypasses both injections.
		test_args.push_back(String("--test-case-exclude=*[SceneTree]*,*[Importer]*"));
	}

	if (test_args.size() > 0) {
		// RAII-owned argv storage. `CharString` owns its UTF-8 buffer (freed
		// in its destructor on scope exit / unwind), so we don't need any
		// explicit cleanup and a throwing applyCommandLine can't leak.
		//
		// Reserve up-front so push_back never reallocates after we start
		// capturing pointers below — LocalVector::push_back reallocates on
		// capacity growth, which would invalidate any prior `ptrw()` pointers.
		LocalVector<CharString> argv_storage;
		argv_storage.reserve(test_args.size());
		for (uint32_t x = 0; x < test_args.size(); x++) {
			argv_storage.push_back(test_args[x].utf8());
		}

		// Capture pointers AFTER all storage push_backs are done so the
		// argv_storage layout is final and stable.
		LocalVector<char *> doctest_argv;
		doctest_argv.reserve(argv_storage.size());
		for (uint32_t x = 0; x < argv_storage.size(); x++) {
			doctest_argv.push_back(argv_storage[x].ptrw());
		}

		ctx.applyCommandLine(int(doctest_argv.size()), doctest_argv.ptr());
	}

	return ctx.run();
}

// Mode flag flipped on by `gs_gpu_test_main` for the lifetime of `_run_doctest`,
// off everywhere else. The listener is registered globally via REGISTER_LISTENER
// (doctest's only registration entry point) but early-returns when this flag is
// false, so the standard `--test` lane is not affected: no double-listener
// registration at priority 1, no spurious `[RID-LEAK?]` print lines when
// SceneTree-only tests transiently spin up an unrelated RD, and no measurement
// against a stale singleton snapshot.
static bool g_in_gs_gpu_mode = false;

// doctest reporter that snapshots GPU memory before and after each test case
// and emits an advisory line when a test leaves > LEAK_BYTES_THRESHOLD bytes
// of GPU memory behind (4 MiB as of #341 — see the threshold's commentary
// in test_case_end for the empirical basis). Registered globally but gated
// by g_in_gs_gpu_mode (see comment above).
struct GsGpuRidLeakListener : public doctest::IReporter {
	explicit GsGpuRidLeakListener(const doctest::ContextOptions &) {}

	uint64_t case_start_memory = 0;
	// #329: remember the case name so a leak advisory can NAME the offender.
	// It used to print `test=advisory`, which made a non-zero total
	// untriageable -- you knew the batch leaked but not which case.
	String case_name;

	void test_case_start(const doctest::TestCaseData &p_data) override {
		case_start_memory = 0;
		case_name = String(p_data.m_name);
		if (!g_in_gs_gpu_mode) {
			return;
		}
#if defined(RD_ENABLED)
		if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
			// #662: settle the device before taking the baseline, exactly as
			// test_case_end does before taking the end sample. Without a drained
			// baseline the two endpoints are not comparable: anything still
			// queued for free here would be released during the case and make
			// the delta negative (silently suppressed), masking a real leak of
			// similar size. test_case_end already drains, so this is normally a
			// no-op -- it matters for the first case after bootstrap, and it
			// keeps the measurement symmetric if a case is ever entered without
			// a preceding drained end.
			const uint32_t drain_cycles = MAX(1u, rd->get_frame_delay()) * 2u;
			for (uint32_t i = 0; i < drain_cycles; i++) {
				rd->swap_buffers(false);
			}
			// #352: measure GS-OWNED bytes (buffers + textures), NOT MEMORY_TOTAL.
			// MEMORY_TOTAL is the VMA grand total and includes process-lifetime
			// driver-internal allocations (compute pipelines, descriptor pools, VMA
			// overhead) that are compiled once and never freed per test case, so it
			// reports a large, order-dependent per-case "leak" even when the test
			// freed every resource it owned. buffers+textures isolates owned data;
			// owned pipeline/shader RID leaks are covered by the lifetime fixture's
			// RDM owned-resource count (and #335's planned handle-count signal).
			case_start_memory = rd->get_memory_usage(RenderingDevice::MEMORY_BUFFERS) + rd->get_memory_usage(RenderingDevice::MEMORY_TEXTURES);
		}
#endif
	}

	void test_case_end(const doctest::CurrentTestCaseStats &) override {
		if (!g_in_gs_gpu_mode) {
			return;
		}
		// #329 (Codex review): drop the SceneDirector's retained SharedWorld
		// renderers BEFORE sampling, or their GPU memory is attributed to
		// whichever case happened to run.
		//
		// The director owns a Ref<GaussianSplatRenderer> per scenario that
		// outlives the case's SceneTree, so without this every [SceneTree] case
		// that built a renderer reported a false multi-hundred-MB leak. Measured
		// on this branch before the fix: NodeSceneTree 2,115,072,932 B,
		// SceneDirectorSceneTree 332,889,640 B, WorldSceneTree 237,081,416 B —
		// and run_gpu_harness.py folds `total_rid_leak_bytes > 0` into
		// gate_failed GLOBALLY (not just for required batches), so those bogus
		// numbers failed the whole harness.
		//
		// This runs AFTER GodotTestCaseListener::test_case_end has torn the
		// SceneTree down (see the priority-0 registration note below), so the
		// nodes' own Refs are already gone and dropping the director's Ref
		// actually frees the renderer instead of just decrementing a refcount.
		if (GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
			director->release_all_worlds();
		}
#if defined(RD_ENABLED)
		// 4 MiB threshold over the GS-owned buffers+textures delta (see
		// test_case_start): the compositor hazard test reports ~2.25 MiB of
		// post-teardown texture pooling (256×256 RGBA8 + D32 + compositor scratch
		// that the RD allocator does not immediately return to the OS). 4 MiB
		// gives a 1.78x margin above that observed noise floor on
		// NVIDIA/Vulkan/release; a genuine missed rd->free for geometry/
		// framebuffer state dwarfs 4 MiB and trips cleanly. Driver variance for
		// AMD/Intel allocator pools is unknown — if a future runner trips this on
		// the canonical hazard test, retune empirically or move to per-batch
		// thresholds via the supervisor's BatchSpec. The listener emits per
		// test_case, so cross-test accumulation is not a concern. See #335 for
		// follow-up to switch to a release-build-friendly handle-count signal.
		constexpr uint64_t LEAK_BYTES_THRESHOLD = 4ull << 20;
		if (RenderingDevice *rd = RenderingDevice::get_singleton()) {
			// #662: DRAIN DEFERRED FREES BEFORE SAMPLING.
			//
			// `MEMORY_BUFFERS`/`MEMORY_TEXTURES` are exact live-object counters
			// (`RenderingDevice::buffer_memory` / `texture_memory`,
			// servers/rendering/rendering_device.h:1580-1581): `+= size` on
			// create, `-= size` on free. They are NOT VMA pool/high-water bytes
			// -- that is `MEMORY_TOTAL`, which this listener deliberately does
			// not use (see test_case_start).
			//
			// But `rd->free()` is DEFERRED: `_free_internal` only queues onto
			// `frames[frame].{buffers,textures}_to_dispose_of`
			// (rendering_device.cpp:6097-6156), and the counters are decremented
			// in `_free_pending_resources()` (6401/6411), which runs ONLY from
			// `_begin_frame()` (6469). On this harness's device nothing ever
			// advances a frame -- there is no swapchain and no main loop -- so a
			// case that correctly frees every RID it created still reads its
			// full allocation as "leaked" at test_case_end.
			//
			// That, not allocator pool growth, is what produced the
			// order-dependent figures in #662: the first case to allocate pays
			// for everything still queued, and a later case reads a baseline
			// that is already inflated, so its own delta goes negative and is
			// suppressed by the `end_memory > case_start_memory` guard below.
			// Excluding the top reporter just moved the same bytes onto the next
			// case (the whack-a-mole #660 observed).
			//
			// `swap_buffers(false)` is the only drain available on a main
			// instance (`submit`/`sync` are local-device only,
			// rendering_device.cpp:6322/6332). It presents nothing; it advances
			// the frame so the pending-free queues are actually processed. Two
			// full cycles over `get_frame_delay()` frames guarantee every queue
			// is drained regardless of frame count.
			//
			// Draining does NOT weaken the gate: a genuinely leaked RID is never
			// queued for free in the first place, so it survives any number of
			// drains and still trips the threshold. This was verified by
			// reintroducing a real leak and confirming the gate still fires --
			// and it is what exposed the real one this change ships a fix for
			// (tile_render_resolve.cpp: external resolved textures freed through
			// the wrong device).
			const uint32_t drain_cycles = MAX(1u, rd->get_frame_delay()) * 2u;
			for (uint32_t i = 0; i < drain_cycles; i++) {
				rd->swap_buffers(false);
			}
			const uint64_t end_memory = rd->get_memory_usage(RenderingDevice::MEMORY_BUFFERS) + rd->get_memory_usage(RenderingDevice::MEMORY_TEXTURES);
			if (end_memory > case_start_memory) {
				const uint64_t delta = end_memory - case_start_memory;
				if (delta > LEAK_BYTES_THRESHOLD) {
					// Format: `[GS-GPU][RID-LEAK?] bytes=N test=<name>`
					// The supervisor (tests/ci/run_gpu_harness.py) scrapes
					// this pattern and folds leaks into gate_failed —
					// listener reporters can't call CHECK_MESSAGE, so we
					// surface the signal via stdout and let the supervisor
					// escalate.
					print_line(vformat("[GS-GPU][RID-LEAK?] bytes=%s test=%s",
							String::num_uint64(delta), case_name));
				}
			}
		}
#endif
	}

	void report_query(const doctest::QueryData &) override {}
	void test_run_start() override {}
	void test_run_end(const doctest::TestRunStats &) override {}
	void test_case_reenter(const doctest::TestCaseData &) override {}
	void test_case_exception(const doctest::TestCaseException &) override {}
	void subcase_start(const doctest::SubcaseSignature &) override {}
	void subcase_end() override {}
	void log_assert(const doctest::AssertData &) override {}
	void log_message(const doctest::MessageData &) override {}
	void test_case_skipped(const doctest::TestCaseData &) override {}
};

// Priority 0, NOT 1 (#329, Codex review). doctest builds its active-listener
// list by iterating getListeners() -- a map keyed by (priority, name) -- and
// inserting each one at reporters_currently_used.begin() (doctest.h:6891-6892),
// which REVERSES map order. Callbacks then run front-to-back (doctest.h:4112).
//
// At priority 1 this listener sorted after "GodotTestCaseListener" in the map
// ("Go" < "Gs"), so after the reversal it ran FIRST -- meaning test_case_end
// sampled GPU memory while the case's SceneTree, its nodes, and every Ref they
// hold were still alive. Every [SceneTree] case therefore looked like a huge
// leak.
//
// Priority 0 sorts this listener FIRST in the map, so the reversal puts it
// LAST: test_case_start now samples after the SceneTree is built and
// test_case_end samples after it is torn down. That is the symmetric,
// meaningful measurement -- SceneTree bring-up cost is excluded from the
// baseline instead of being counted as a leak at the end.
//
// If a third listener is ever added, re-derive the order rather than assuming
// it; the insert-at-begin reversal is easy to get backwards.
REGISTER_LISTENER("GsGpuRidLeakListener", 0, GsGpuRidLeakListener);

} // namespace

int gs_gpu_test_main(int argc, char *argv[]) {
#if !defined(RD_ENABLED)
	(void)argc;
	(void)argv;
	print_error("[GS-GPU] FAIL: build is missing RD_ENABLED; no RenderingDevice support compiled");
	return GS_GPU_EXIT_RD_NOT_COMPILED;
#else
	Error setup_err = Main::test_setup();
	if (setup_err != OK) {
		print_error(vformat("[GS-GPU] FAIL: Main::test_setup() returned %d", int(setup_err)));
		return GS_GPU_EXIT_FAIL_TEST_SETUP;
	}

	// Issue #392: Main::test_setup() creates the WorkerThreadPool singleton but
	// does NOT start its threads, so the pool's ThreadData vector is empty. Renderer
	// lifetime scenarios (renderer_instance / asset_attach_detach) transitively index
	// that vector via GaussianSplatManager::register_gaussian_buffer /
	// _request_primary_local_device and OOB-crash (STATUS_STACK_BUFFER_OVERRUN) without
	// it. Start the pool here exactly as tests/test_main.cpp:242 does (engine teardown
	// in Main::test_cleanup() finishes it), so the Lifetime GPU harness batch runs its
	// assertions instead of skipping via REQUIRE_WORKER_THREAD_POOL().
	WorkerThreadPool::get_singleton()->init();

	// Issue #329: register the mock DisplayServer driver exactly as
	// tests/test_main.cpp:240 does. `gs_gpu_test_main` bypasses `test_main()`
	// entirely (it owns its own doctest::Context), so without this call the
	// "mock" driver is never registered.
	//
	// That absence — not a missing SceneTree — is what made every
	// [SceneTree]+[RequiresGPU] case crash under this harness. The globally
	// registered GodotTestCaseListener (tests/test_main.cpp:333) DOES build a
	// full SceneTree for [SceneTree]-tagged cases here, but it first scans
	// DisplayServer::get_create_function_count() for a driver named "mock" and
	// silently creates nothing when it is absent. It then calls
	// RenderingServerDefault::init() with no DisplayServer behind it, and
	// RendererCompositor::create() dereferences null:
	//
	//   [2] RendererCompositor::create
	//   [3] RenderingServerDefault::_init
	//   [4] RenderingServerDefault::init
	//   [5] GodotTestCaseListener::test_case_start
	//
	// Registering the driver lets the listener take its normal path, so the
	// deferred cases run here instead of crashing. Cheap and inert for
	// non-[SceneTree] cases: registration only adds a create-function entry;
	// nothing is instantiated unless a [SceneTree]/[Editor] case asks for it.
	// (test_main.cpp:239 additionally calls OS::set_cmdline(); that member is
	// protected and only reachable from test_main's friend context, and no
	// deferred case reads OS::get_cmdline_args(), so it is deliberately not
	// mirrored here.)
	DisplayServerMock::register_mock_driver();

	const GsGpuTestOptions opt = _parse_options(argc, argv);

	int bootstrap_rc = _bootstrap_rd(opt);
	if (bootstrap_rc != 0) {
		Main::test_cleanup();
		return bootstrap_rc;
	}

	g_in_gs_gpu_mode = true;
	int rc = _run_doctest(argc, argv, opt);
	g_in_gs_gpu_mode = false;

	// Issue #329: three-phase teardown. The two obvious orderings each crash,
	// because the lifetime constraints are circular:
	//
	//   * `_teardown_rd()` BEFORE `Main::test_cleanup()` (the original order)
	//     kills the RenderingDevice while the GaussianSplatSceneDirector still
	//     owns SharedWorld renderers. Module uninitialization inside
	//     test_cleanup() then frees their GPU resources through a dangling
	//     device:
	//         [0] RenderingDevice::_compute_list_set_push_constant
	//         [2] ShaderRD::ShaderRD
	//         [5] GaussianSplatting::TileShaderResources::reset_state
	//         [7] TileRenderer::_release_compiled_shaders
	//        [10] GaussianSplatRenderer::~GaussianSplatRenderer
	//        [22] GaussianSplatSceneDirector::~GaussianSplatSceneDirector
	//        [27] Main::test_cleanup
	//
	//   * `Main::test_cleanup()` BEFORE `_teardown_rd()` inverts the problem:
	//     test_cleanup() deletes the Engine singleton, and
	//     `RenderingDevice::~RenderingDevice` reads it during finalize:
	//         [0] Engine::is_extra_gpu_memory_tracking_enabled
	//         [2] RenderingDeviceDriverVulkan::pipeline_free
	//         [3] RenderingDevice::_free_pending_resources
	//         [6] RenderingDevice::finalize
	//         [7] RenderingDevice::~RenderingDevice
	//        [10] _teardown_rd
	//
	// So: drop the module's device-backed state FIRST (device + manager both
	// still live, which is the order #589 requires), THEN the device, THEN the
	// engine. The pre-existing batches never hit this because they release
	// their renderers inside the test body; only a SceneDirector-owned
	// SharedWorld -- what a [SceneTree] case builds -- survives the test case.
	if (GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
		director->release_all_worlds();
	}
	_teardown_rd();
	Main::test_cleanup();
	return rc;
#endif
}

#endif // TESTS_ENABLED
