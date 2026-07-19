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

// #611 PR B1 — register_instance's `world->renderer->initialize()` was a FIFTH
// blocking dispatch under world_mutex, on a route neither instrumented boundary
// could see (so PR A's counter was a lower bound). It is now deferred like the
// applies.
//
// Same limitation as everything above: this pins the *bookkeeping*, not a stall.
// What makes the initialize case different from the applies is that it carries a
// guard -- `!gpu_resources_initialized && !gpu_initialization_pending` -- which
// was atomic with the call while both sat under the lock. Deferring separates
// them, so the guard has to be re-evaluated at dispatch time. Both halves of
// that (the re-check, and the front-insertion that keeps initialize ahead of an
// already-queued apply) are asserted here.
TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] DeferredRendererWork defers initialize ahead of queued applies") {
	Ref<GaussianSplatRenderer> renderer(memnew(GaussianSplatRenderer(nullptr)));
	if (renderer.is_null()) {
		FAIL("could not construct a device-less GaussianSplatRenderer; the deferred "
			 "initialize path cannot be exercised without one");
		return;
	}
	// A device-less renderer must start un-initialized, or the guard re-check
	// below would skip and the test would assert nothing.
	if (renderer->get_resource_state().gpu_resources_initialized ||
			renderer->get_resource_state().gpu_initialization_pending) {
		FAIL("a freshly constructed device-less renderer already reports initialized/pending; "
			 "the guard re-check cannot be discriminated");
		return;
	}

	{
		// ORDERING. This is the trap PR A hit in a different shape: a naively
		// appended entry runs after work queued earlier. register_instance's
		// initialize ran inline under the lock, i.e. BEFORE the apply that
		// _get_or_create_world_for_scenario queues when it lazily creates the same
		// renderer. Appending would invert that and apply a contract to a renderer
		// whose GPU resources are not up.
		//
		// No side effect can distinguish the two orders headless (a device-less
		// initialize() and apply() converge on the same resource state), so this
		// asserts dispatch order directly.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_apply(renderer, GaussianSplatRenderer::WorldSubmissionContract());
		work.queue_initialize(renderer);
		CHECK(work.size() == 2u);
		CHECK(work.get_entry_kind(0) == GaussianSplatSceneDirector::DeferredRendererWork::Kind::INITIALIZE);
		CHECK(work.get_entry_kind(1) == GaussianSplatSceneDirector::DeferredRendererWork::Kind::APPLY);
		work.cancel();
	}

	{
		// A null renderer must not queue a dangling initialize, matching
		// queue_apply / queue_restore.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_initialize(Ref<GaussianSplatRenderer>());
		CHECK(work.is_empty());
	}

	{
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_initialize(renderer);
		CHECK(work.size() == 1u);
		// One Ref per entry, as with the other kinds: this is what keeps the
		// renderer alive across the unlock if its world is pruned in the gap.
		CHECK(renderer->get_reference_count() == 2);

		work.cancel();
		CHECK(work.is_empty());
		CHECK(renderer->get_reference_count() == 1);
		work.flush();
		// Cancelled initialize does not run -- so a cancel cannot silently drop
		// GPU bring-up while claiming it happened.
		CHECK(work.get_dispatched_entry_count() == 0u);
	}

	{
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_initialize(renderer);
		work.flush();
		CHECK(work.is_empty());
		// The deferral must not lose the initialization it moved out of the
		// critical section.
		CHECK(work.get_dispatched_entry_count() == 1u);
		CHECK(renderer->get_reference_count() == 1);
	}

	// The device-less initialize above ran `_create_gpu_resources_safe()`, which
	// sets gpu_initialization_pending before it discovers there is no
	// RenderingDevice (renderer/render_resource_orchestrator.cpp). That is what
	// makes the guard re-check observable headless at all.
	CHECK(renderer->get_resource_state().gpu_initialization_pending);

	{
		// GUARD RE-CHECK. The renderer is now pending, so a queued initialize must
		// be skipped at flush time rather than re-running -- and a skip must not be
		// counted as dispatched, or this assertion could not tell the two apart.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_initialize(renderer);
		CHECK(work.size() == 1u);
		work.flush();
		CHECK(work.get_dispatched_entry_count() == 0u);
	}
}

// #611 PR B2 — submit_world_submission's three-phase restructure
// (arbitrate-under-lock -> apply-unlocked -> commit-under-lock).
//
// SAME LIMITATION AS EVERYTHING ABOVE, and it is worth restating because this is
// the case people most want a behavioural test for: nothing here proves the
// stall is gone. It cannot. The two behaviours the restructure has to preserve —
// `set_max_splats` silently dropped, `set_gaussian_data` rolled back and
// rejected — are produced by a *dispatch timeout*, and headless never dispatches
// at all. Asserting either one here would pass identically on a tree with the
// inversion fully live.
//
// What IS provable headless is the queue bookkeeping the restructure newly
// depends on. Phase 3 no longer dispatches its rollback inline; it queues it,
// behind whatever `_get_or_create_world_for_scenario` may already have queued in
// phase 1. So the FINAL renderer state after a rejection is decided entirely by
// flush order, and these two orderings are what make the reject and commit paths
// correct. Both are real invariants: the second one is a bug the implementation
// hit before it was written down.
//
// The structural proof that the inversion itself is gone lives in
// tests/ci/check_renderer_contract_boundary.py, whose caller-side rule reports 3
// violations in submit_world_submission on the pre-fix tree and 0 after.
TEST_CASE("[GaussianSplatting][SceneDirector][Lifetime] DeferredRendererWork ordering pins submit_world_submission's reject and commit paths") {
	Ref<GaussianSplatRenderer> renderer(memnew(GaussianSplatRenderer(nullptr)));
	if (renderer.is_null()) {
		FAIL("could not construct a device-less GaussianSplatRenderer; the queue "
			 "ordering the three-phase restructure relies on cannot be exercised");
		return;
	}
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot cleared;
	if (cleared.valid) {
		FAIL("a default-constructed snapshot reports valid; the restore below would "
			 "take the restore_world_submission_runtime_state path instead of the "
			 "device-free clear path and this test would not be self-contained");
		return;
	}

	{
		// REJECT PATH ordering. Phase 1 may queue an apply of the PREVIOUS record P
		// (when it lazily creates the renderer); phase 3's rejection then adds the
		// rollback. cancel() is deliberately NOT called on this path, so both
		// entries survive — and the ROLLBACK MUST RUN FIRST, leaving apply(P) as the
		// last writer.
		//
		// This is the opposite of what an earlier draft of this test asserted, and
		// the correction came from review (Codex on #674) rather than from any lane
		// here. The reasoning: a rejection leaves the director's record at P, so the
		// renderer must end up holding P's contract or the two diverge. The
		// rollback's snapshot was taken from the still-blank renderer BEFORE the
		// queued apply(P) ran, so if the rollback went last it would undo P and
		// leave the renderer cleared under a live record. The pre-#611 code restored
		// inline and flushed apply(P) afterwards; queue_restore_first reproduces
		// exactly that order.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_apply(renderer, GaussianSplatRenderer::WorldSubmissionContract());
		work.queue_restore_first(renderer, cleared);
		CHECK(work.size() == 2u);
		// The load-bearing assertion: rollback BEFORE the stale apply, so apply(P)
		// is the final word.
		CHECK(work.get_entry_kind(0) == GaussianSplatSceneDirector::DeferredRendererWork::Kind::RESTORE);
		CHECK(work.get_entry_kind(1) == GaussianSplatSceneDirector::DeferredRendererWork::Kind::APPLY);

		work.flush();
		// Neither entry may be dropped: the reject path's correctness is that BOTH
		// run, in this order.
		CHECK(work.get_dispatched_entry_count() == 2u);
		CHECK(work.is_empty());
	}

	{
		// With nothing already queued, queue_restore_first must degenerate to a
		// plain append — otherwise the common rejection (renderer already existed,
		// so phase 1 queued no apply) would depend on which entry point was used.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_restore_first(renderer, cleared);
		CHECK(work.size() == 1u);
		CHECK(work.get_entry_kind(0) == GaussianSplatSceneDirector::DeferredRendererWork::Kind::RESTORE);
		work.flush();
		CHECK(work.get_dispatched_entry_count() == 1u);
	}

	{
		// A null renderer must not queue a dangling entry, matching the other kinds.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_restore_first(Ref<GaussianSplatRenderer>(), cleared);
		CHECK(work.is_empty());
	}

	{
		// COMMIT PATH. cancel() clears the ENTIRE queue, not just the superseded
		// apply it is aimed at. The commit branch also has to queue a restore for the
		// evicted previous world, so the two operations are order-sensitive:
		// cancel() must come FIRST.
		//
		// This is not hypothetical — writing the commit branch with the queue_restore
		// ahead of the cancel() silently dropped that restore, leaving the evicted
		// world's renderer holding a contract it no longer owned.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_apply(renderer, GaussianSplatRenderer::WorldSubmissionContract());
		work.cancel();
		work.queue_restore(renderer, cleared);
		CHECK(work.size() == 1u);
		CHECK(work.get_entry_kind(0) == GaussianSplatSceneDirector::DeferredRendererWork::Kind::RESTORE);

		work.flush();
		// Exactly one: the superseded apply is gone, the previous-world restore is
		// not.
		CHECK(work.get_dispatched_entry_count() == 1u);
	}

	{
		// ...and the inverse order, which is the bug the branch above must not
		// reintroduce: cancel() after queueing takes the restore with it.
		GaussianSplatSceneDirector::DeferredRendererWork work;
		work.queue_apply(renderer, GaussianSplatRenderer::WorldSubmissionContract());
		work.queue_restore(renderer, cleared);
		work.cancel();
		CHECK(work.is_empty());
		work.flush();
		CHECK(work.get_dispatched_entry_count() == 0u);
	}

	// No entry may outlive the queue: every Ref taken above is released again, or
	// the renderer would leak past the submission that borrowed it.
	CHECK(renderer->get_reference_count() == 1);
}

} // namespace TestSceneDirectorRendererContractLock

#endif // TEST_SCENE_DIRECTOR_RENDERER_CONTRACT_LOCK_H
