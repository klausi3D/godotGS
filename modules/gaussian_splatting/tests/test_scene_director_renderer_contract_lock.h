/**************************************************************************/
/*  test_scene_director_renderer_contract_lock.h                          */
/**************************************************************************/
// #611 — structural coverage for the world_mutex <-> render-thread lock-order
// inversion.
//
// WHAT IS NOT TESTED HERE, AND WHY
//
// Nothing in this file reproduces the actual stall. It cannot, and neither can
// any other lane in this repository:
//
//   * every doctest process is launched as `--headless --test`
//     (tests/ci/run_module_tests.py:350);
//   * under headless, RenderThreadDispatcher short-circuits before it ever
//     blocks — it returns false on `is_on_render_thread()` or
//     `!is_render_loop_enabled()` (interfaces/render_thread_dispatcher.cpp:17-22
//     for the blocking dispatch, :116-122 for the would-block predicate);
//   * the `--gs-gpu-test` harness boots a RenderingDevice with no SceneTree, so
//     the director path is never driven there either.
//
// A test asserting "the submission stalled" or "the setting was dropped" would
// therefore pass on a build where the inversion is fully live, which is exactly
// the vacuous-green failure mode this module keeps rediscovering. So the
// coverage here is deliberately *structural*: it pins the enforcement mechanism
// that lets a live run report the violation itself.

#ifndef TEST_SCENE_DIRECTOR_RENDERER_CONTRACT_LOCK_H
#define TEST_SCENE_DIRECTOR_RENDERER_CONTRACT_LOCK_H

#include "tests/test_macros.h"

#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

#include "../core/gaussian_splat_scene_director.h"
#include "../core/thread_owned_mutex.h"

namespace TestSceneDirectorRendererContractLock {

TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] ThreadOwnedMutex reports ownership only inside the critical section") {
	GaussianSplatting::ThreadOwnedMutex mutex;

	CHECK_FALSE(mutex.is_held_by_current_thread());
	CHECK(mutex.get_owned_recursion_depth() == 0u);

	{
		GaussianSplatting::ThreadOwnedMutexLock outer(mutex);
		CHECK(mutex.is_held_by_current_thread());
		CHECK(mutex.get_owned_recursion_depth() == 1u);

		{
			// The director's world_mutex is recursive, so ownership must survive
			// an inner scope's release.
			GaussianSplatting::ThreadOwnedMutexLock inner(mutex);
			CHECK(mutex.is_held_by_current_thread());
			CHECK(mutex.get_owned_recursion_depth() == 2u);
		}

		CHECK(mutex.is_held_by_current_thread());
		CHECK(mutex.get_owned_recursion_depth() == 1u);
	}

	CHECK_FALSE(mutex.is_held_by_current_thread());
	CHECK(mutex.get_owned_recursion_depth() == 0u);
}

struct CrossThreadOwnershipProbe {
	GaussianSplatting::ThreadOwnedMutex mutex;
	Semaphore main_has_locked;
	Semaphore worker_observed;
	Semaphore worker_has_locked;
	Semaphore main_observed;
	// Seeded to the values that would indicate a broken guard, so a worker that
	// never runs cannot produce a green result.
	bool worker_saw_ownership_while_main_held = true;
	bool worker_saw_ownership_while_worker_held = false;
};

static void _cross_thread_ownership_body(void *p_userdata) {
	CrossThreadOwnershipProbe *probe = static_cast<CrossThreadOwnershipProbe *>(p_userdata);
	if (!probe) {
		return;
	}

	probe->main_has_locked.wait();
	// The main thread holds the mutex: this thread must not claim ownership.
	probe->worker_saw_ownership_while_main_held = probe->mutex.is_held_by_current_thread();
	probe->worker_observed.post();

	probe->mutex.lock();
	probe->worker_saw_ownership_while_worker_held = probe->mutex.is_held_by_current_thread();
	probe->worker_has_locked.post();
	probe->main_observed.wait();
	probe->mutex.unlock();
}

TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] ThreadOwnedMutex ownership is per-thread, not global") {
	CrossThreadOwnershipProbe probe;

	Thread worker;
	worker.start(_cross_thread_ownership_body, &probe);

	probe.mutex.lock();
	CHECK(probe.mutex.is_held_by_current_thread());
	probe.main_has_locked.post();
	probe.worker_observed.wait();
	CHECK_FALSE(probe.worker_saw_ownership_while_main_held);
	probe.mutex.unlock();
	CHECK_FALSE(probe.mutex.is_held_by_current_thread());

	probe.worker_has_locked.wait();
	// The worker now owns the mutex. The main thread must NOT report ownership —
	// this is the assertion that distinguishes real per-thread tracking from a
	// plain "is anyone holding it" flag, which is what makes the boundary check
	// meaningful for the main-thread/render-thread pair this guard exists for.
	CHECK_FALSE(probe.mutex.is_held_by_current_thread());
	CHECK(probe.worker_saw_ownership_while_worker_held);
	probe.main_observed.post();

	worker.wait_to_finish();
}

TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] renderer-contract boundary check fires only while the lock is held") {
	GaussianSplatting::ThreadOwnedMutex mutex;
	SafeNumeric<uint64_t> violations{ 0 };

	CHECK_FALSE(GaussianSplatting::report_lock_held_at_boundary(mutex, violations));
	CHECK(violations.get() == 0u);

	{
		GaussianSplatting::ThreadOwnedMutexLock lock(mutex);
		// This is the shape of the real defect: crossing the renderer-contract
		// boundary (a blocking render-thread dispatch) from inside the critical
		// section the render thread itself needs.
		CHECK(GaussianSplatting::report_lock_held_at_boundary(mutex, violations));
	}
	CHECK(violations.get() == 1u);

	// ...and it must go quiet again once the lock is released, or the counter
	// would be noise rather than a signal.
	CHECK_FALSE(GaussianSplatting::report_lock_held_at_boundary(mutex, violations));
	CHECK(violations.get() == 1u);
}

TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] director exposes a resettable renderer-contract violation count") {
	// The director's counter is the live-run self-report for the violation that
	// remains in submit_world_submission (#611 PR B). This pins its contract; it
	// deliberately does not assert a *value* produced by driving the director,
	// because no renderer can be created in this lane (no RenderingDevice) and
	// such an assertion would be green whether or not the inversion exists.
	GaussianSplatSceneDirector::reset_renderer_contract_lock_violation_count();
	CHECK(GaussianSplatSceneDirector::get_renderer_contract_lock_violation_count() == 0u);
}

} // namespace TestSceneDirectorRendererContractLock

#endif // TEST_SCENE_DIRECTOR_RENDERER_CONTRACT_LOCK_H
