#ifndef GAUSSIAN_SPLAT_THREAD_OWNED_MUTEX_H
#define GAUSSIAN_SPLAT_THREAD_OWNED_MUTEX_H

#include "core/error/error_macros.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

namespace GaussianSplatting {

// #611: a `Mutex` that remembers which thread currently owns it.
//
// GaussianSplatSceneDirector::world_mutex sits on both sides of a lock-order
// inversion: the main thread takes it and then issues a *blocking* render-thread
// dispatch (renderer teardown, `initialize()`, `set_max_splats`,
// `set_gaussian_data`, `set_file_backed_payload_source`), while the render thread
// itself needs the same mutex inside the director's `*_for_renderer` builders. A
// blocking dispatch under the lock therefore stalls until the dispatch timeout
// expires and then silently drops (or rolls back) the operation.
//
// The rule "never cross the renderer-contract boundary while holding
// world_mutex" used to live only in a header comment, with every caller required
// to get the ordering right independently. This type makes the rule *queryable*
// at runtime, so the boundary itself can check it instead of being trusted.
//
// Ownership tracking is deliberately conservative: `Thread::get_caller_id()`
// returns `Thread::UNASSIGNED_ID` on threads that Godot's `Thread` never
// registered, and two such threads are indistinguishable from each other. When
// the caller cannot be identified, `is_held_by_current_thread()` reports
// *false* — it would rather miss a violation than invent one, because a false
// positive here would be indistinguishable from the real defect. Both threads
// this guard exists for (the main thread and the RenderingServer render thread)
// are registered `Thread`s and are therefore always identifiable.
class ThreadOwnedMutex {
	mutable Mutex mutex; // Recursive, matching the `Mutex` this replaces.
	mutable SafeNumeric<uint64_t> owner_id{ Thread::UNASSIGNED_ID };
	// Only ever touched by the owning thread while `mutex` is held, so a plain
	// integer is sufficient; nothing outside the critical section reads it.
	mutable uint32_t recursion_depth = 0;

public:
	_ALWAYS_INLINE_ void lock() const {
		mutex.lock();
		recursion_depth++;
		owner_id.set(Thread::get_caller_id());
	}

	_ALWAYS_INLINE_ void unlock() const {
		if (recursion_depth > 0) {
			recursion_depth--;
		}
		if (recursion_depth == 0) {
			// Published before the mutex is released so no incoming owner can
			// observe a stale id.
			owner_id.set(Thread::UNASSIGNED_ID);
		}
		mutex.unlock();
	}

	_ALWAYS_INLINE_ bool is_held_by_current_thread() const {
		const Thread::ID caller = Thread::get_caller_id();
		if (caller == Thread::UNASSIGNED_ID) {
			return false;
		}
		return owner_id.get() == caller;
	}

	// Recursion depth as seen by the owning thread; 0 for anyone else.
	_ALWAYS_INLINE_ uint32_t get_owned_recursion_depth() const {
		return is_held_by_current_thread() ? recursion_depth : 0u;
	}

	ThreadOwnedMutex() = default;
	ThreadOwnedMutex(const ThreadOwnedMutex &) = delete;
	ThreadOwnedMutex &operator=(const ThreadOwnedMutex &) = delete;
};

// RAII lock for ThreadOwnedMutex.
//
// Godot's `MutexLock` binds a `std::unique_lock` straight onto `MutexImpl`'s
// private `std::mutex`, bypassing `lock()`/`unlock()` entirely, so it cannot
// maintain the ownership record. This type must be used instead.
class ThreadOwnedMutexLock {
	const ThreadOwnedMutex &mutex;

public:
	explicit ThreadOwnedMutexLock(const ThreadOwnedMutex &p_mutex) :
			mutex(p_mutex) {
		mutex.lock();
	}
	~ThreadOwnedMutexLock() {
		mutex.unlock();
	}

	ThreadOwnedMutexLock(const ThreadOwnedMutexLock &) = delete;
	ThreadOwnedMutexLock &operator=(const ThreadOwnedMutexLock &) = delete;
};

// The renderer-contract boundary check.
//
// Returns true when `p_mutex` is held by the calling thread — i.e. when the
// caller is about to issue a blocking render-thread dispatch from inside the
// critical section the render thread itself needs. The violation is counted in
// `r_violation_count` so a caller (or a test) can observe it without parsing
// logs.
//
// This *reports*; it does not abort. Aborting would change behaviour on the one
// call path that is still knowingly in violation (`submit_world_submission`'s
// apply, deferred to the follow-up PR), turning every world submission on a live
// render thread into a rejection. Reporting is what makes that remaining
// violation self-describing instead of merely argued — there is no lane in this
// repo that can reproduce the stall behaviourally (every doctest run is
// `--headless --test`, under which the dispatcher short-circuits and never
// blocks).
_ALWAYS_INLINE_ bool report_lock_held_at_boundary(const ThreadOwnedMutex &p_mutex,
		SafeNumeric<uint64_t> &r_violation_count) {
	if (!p_mutex.is_held_by_current_thread()) {
		return false;
	}
	r_violation_count.increment();
	return true;
}

} // namespace GaussianSplatting

#endif // GAUSSIAN_SPLAT_THREAD_OWNED_MUTEX_H
