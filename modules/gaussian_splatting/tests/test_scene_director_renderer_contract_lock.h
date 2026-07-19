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

#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

#include "../core/gaussian_splat_scene_director.h"
#include "../core/thread_owned_mutex.h"

namespace TestSceneDirectorRendererContractLock {

// Bounded replacement for Semaphore::wait(). An unbounded wait turns a worker
// that never starts into a hung lane rather than a failed assertion, which is
// strictly worse than a red test.
static bool wait_bounded(const Semaphore &p_semaphore, uint32_t p_timeout_msec = 5000) {
	OS *os = OS::get_singleton();
	if (!os) {
		return p_semaphore.try_wait();
	}
	const uint64_t deadline = os->get_ticks_msec() + p_timeout_msec;
	while (!p_semaphore.try_wait()) {
		if (os->get_ticks_msec() >= deadline) {
			return false;
		}
		os->delay_usec(1000);
	}
	return true;
}

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
	bool worker_waits_timed_out = false;
};

static void _cross_thread_ownership_body(void *p_userdata) {
	CrossThreadOwnershipProbe *probe = static_cast<CrossThreadOwnershipProbe *>(p_userdata);
	if (!probe) {
		return;
	}

	// Every wait here is bounded, and every post still happens on the timeout
	// path, so the main thread's join can never block indefinitely. doctest
	// assertions are not thread-safe, so failures are recorded and checked on the
	// main thread instead.
	if (!wait_bounded(probe->main_has_locked)) {
		probe->worker_waits_timed_out = true;
		probe->worker_observed.post();
		return;
	}
	// The main thread holds the mutex: this thread must not claim ownership.
	probe->worker_saw_ownership_while_main_held = probe->mutex.is_held_by_current_thread();
	probe->worker_observed.post();

	probe->mutex.lock();
	probe->worker_saw_ownership_while_worker_held = probe->mutex.is_held_by_current_thread();
	probe->worker_has_locked.post();
	if (!wait_bounded(probe->main_observed)) {
		probe->worker_waits_timed_out = true;
	}
	probe->mutex.unlock();
}

TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] ThreadOwnedMutex ownership is per-thread, not global") {
	CrossThreadOwnershipProbe probe;

	Thread worker;
	worker.start(_cross_thread_ownership_body, &probe);

	probe.mutex.lock();
	CHECK(probe.mutex.is_held_by_current_thread());
	probe.main_has_locked.post();
	const bool observed_under_main_lock = wait_bounded(probe.worker_observed);
	CHECK(observed_under_main_lock);
	if (observed_under_main_lock) {
		CHECK_FALSE(probe.worker_saw_ownership_while_main_held);
	}
	probe.mutex.unlock();
	CHECK_FALSE(probe.mutex.is_held_by_current_thread());

	if (observed_under_main_lock && wait_bounded(probe.worker_has_locked)) {
		// The worker now owns the mutex. The main thread must NOT report ownership
		// — this is the assertion that distinguishes real per-thread tracking from
		// a plain "is anyone holding it" flag, which is what makes the boundary
		// check meaningful for the main-thread/render-thread pair this guard
		// exists for.
		CHECK_FALSE(probe.mutex.is_held_by_current_thread());
		CHECK(probe.worker_saw_ownership_while_worker_held);
	} else if (observed_under_main_lock) {
		FAIL("worker never acquired the mutex within the timeout");
	}

	// Posted unconditionally: the worker may already be waiting on it, and the
	// join below must not depend on which branch above ran.
	probe.main_observed.post();
	worker.wait_to_finish();
	CHECK_FALSE(probe.worker_waits_timed_out);
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

// The deferral's correctness rests on two pieces of pure bookkeeping:
//
//   1. flush() actually dispatches what was queued — otherwise the applies this
//      change moved out of the critical section would simply be lost;
//   2. cancel() actually suppresses it — this is what stops
//      submit_world_submission's committed contract from being clobbered by the
//      superseded apply that _get_or_create_world_for_scenario queued.
//
// Neither needs a RenderingDevice, so both are tested here directly.
//
// NOT covered, and not coverable in any current lane: the *wiring* — that the
// commit path calls cancel() and the two failure exits do not. Reaching it
// requires the director to lazily create a renderer, which requires a
// RenderingDevice that no headless lane has. Deleting the cancel() call at the
// commit site leaves every test in this file green.
TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] DeferredRendererWork dispatches on flush and suppresses on cancel") {
	Ref<GaussianSplatRenderer> renderer(memnew(GaussianSplatRenderer(nullptr)));
	if (renderer.is_null()) {
		FAIL("could not construct a device-less GaussianSplatRenderer; the queue's "
			 "dispatch/suppress behaviour cannot be exercised without one");
		return;
	}
	CHECK(renderer->get_reference_count() == 1);

	// An invalid snapshot routes flush() to clear_world_submission_contract(),
	// the cheapest queued operation that needs no GPU resources.
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot cleared;
	CHECK_FALSE(cleared.valid);

	{
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_restore(renderer, cleared);
		work.queue_restore(renderer, cleared);
		CHECK(work.size() == 2u);
		// One Ref per entry: this is what keeps the renderer alive across the
		// unlock even when the world it came from is pruned in the gap.
		CHECK(renderer->get_reference_count() == 3);

		work.cancel();
		CHECK(work.is_empty());
		CHECK(renderer->get_reference_count() == 1);
		work.flush();
		// The load-bearing assertion for claim 2: cancelled work does not run,
		// even though flush() is still invoked (by the destructor, at every call
		// site).
		CHECK(work.get_dispatched_entry_count() == 0u);
	}

	{
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_restore(renderer, cleared);
		CHECK(work.size() == 1u);
		work.flush();
		CHECK(work.is_empty());
		// The load-bearing assertion for claim 1: queued work is not silently
		// dropped by the deferral.
		CHECK(work.get_dispatched_entry_count() == 1u);
		CHECK(renderer->get_reference_count() == 1);
	}

	{
		// A world pruned before its renderer was captured must not queue a
		// dangling entry.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_restore(Ref<GaussianSplatRenderer>(), cleared);
		work.queue_apply(Ref<GaussianSplatRenderer>(), GaussianSplatRenderer::WorldSubmissionContract());
		CHECK(work.is_empty());
		work.flush();
		CHECK(work.get_dispatched_entry_count() == 0u);
	}
}

} // namespace TestSceneDirectorRendererContractLock

#endif // TEST_SCENE_DIRECTOR_RENDERER_CONTRACT_LOCK_H
