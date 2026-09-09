#pragma once

// #794: checked Vector::resize() for the "resize then write[]" pattern.
//
// Vector<T>::resize() reports allocation failure through its RETURN VALUE and leaves
// the vector at its previous, smaller size: CowData::resize() -> _fork_allocate()
// hits ERR_FAIL_NULL_V(mem_new, ERR_OUT_OF_MEMORY) and returns before size() is ever
// touched (core/templates/cowdata.h). Code that ignores that return and then writes
// vec.write[i] for i < requested_count walks off the end, and CRASH_BAD_INDEX in
// core/templates/vector.h hard-traps the whole process via __fastfail(7).
//
// CRASH_BAD_INDEX is NOT gated on DEBUG_ENABLED, so this is a hard crash in EVERY
// build configuration including release -- not a latent corruption. It is exactly how
// the nightly GPU Streaming Stress lane died in #787 (0xC0000409, fast-fail subcode 7,
// symbolized to VectorWriteProxy<PackedGaussian>::operator[]).
//
// The read path traps identically: Vector::operator[] const -> CowData::get() also
// asserts CRASH_BAD_INDEX, so a site whose write loop is bounded by the vector's own
// size() can still trap on a later indexed read.
//
// This generalises the file-local _reserve_pack_output() that #793 introduced for the
// pack sites. It deliberately keeps that helper's two properties:
//
//   1. Report count, element size and total bytes. That is the diagnostic the crash
//      itself can never deliver, which is the whole reason #787 needed symbolization.
//   2. Leave the output EMPTY, never partially sized, so no caller can consume a
//      truncated buffer. Trading a loud crash for silent data loss would be worse
//      than the bug.
//
// [[nodiscard]] is load-bearing, not decoration: #793 established that making a
// function fail closed is not enough if a caller ignores the result -- three of its
// four callers consumed the empty output, one handing buffer_update() a null pointer
// with a nonzero byte count. Ignoring this result is a compile error.
//
// ---------------------------------------------------------------------------
// SCOPE RULE: which resize() calls need this
// ---------------------------------------------------------------------------
// A call needs checking when the index written afterwards is bounded by the
// REQUESTED count rather than by the vector's own size(). Two exclusions follow from
// that, and they are rules, not a curated exception list -- which matters, because an
// invariant guarded by a hand-maintained list is already broken:
//
//   1. The index is bounded by the vector's own size(). A failed resize shrinks the
//      bound along with the buffer, so the write stays in range. NOTE this covers the
//      write only: a *later* indexed read against a different bound can still trap
//      (see the pair in gaussian_streaming.cpp vs render_streaming_orchestrator.cpp,
//      where one traps and one does not, for exactly this reason).
//   2. The count is a compile-time constant that does not scale with scene, asset or
//      file data (a literal, or a fixed ring-buffer capacity). The index then cannot
//      exceed what a successful resize produced, and a failure means the allocator
//      could not serve a handful of small structs -- the process is already gone.
//      Sites excluded this way carry a one-line comment naming this rule.
//
// Everything else -- any count derived from splat counts, chunk counts, file headers,
// or collection sizes -- is in scope. Those are the allocations that can realistically
// fail, and #787 is the proof that they do.

#include "core/error/error_macros.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
// vformat() is declared in variant.h, and it MUST be included here: this is a
// template, so under GCC's two-phase name lookup a call with no argument depending on
// the template parameter has to be resolvable at definition time. MSVC's permissive
// lookup accepts it without the include, so omitting this builds on Windows and fails
// only on the Linux lane -- which is exactly how it was found.
#include "core/variant/variant.h"

#if defined(TESTS_ENABLED)
#include <atomic>
#include <cstring>

// #798 review round 3: a test cannot reach the FAILURE branch below by choosing a large
// count. Every caller in scope validates its count first, and any count big enough to
// actually exhaust the allocator is rejected by that validation long before it gets here
// -- proven on a sibling PR. Without a seam, a test that claims to cover a fail-closed
// rollback path in fact runs the SUCCESS path and passes with the rollback deleted.
//
// So arm one specific call site by its `p_where` label and let the real
// gs_resize_or_fail() take its real failure branch. The test drives the code under test,
// not a test-only shortcut around it.
//
// Properties that make this safe to have in the tree:
//   * Compiled out entirely unless TESTS_ENABLED. Nothing ships (#725 shipped test hooks
//     in a release build once; this must not repeat).
//   * Matched by label, not "next call". A one-shot "fail the next resize" latch would be
//     consumed by whatever unrelated allocation happens to run between arming and the
//     target -- non-deterministic, and silently mis-aimed.
//   * One-shot: the arm is cleared by the firing, so a single armed site cannot leak into
//     later cases in the same process.
//   * Atomic pointer, no allocation and no String work on the disarmed path, so an armed
//     test cannot race a worker thread that also allocates.
// The armed pointer must have static storage duration (pass a string literal).
inline std::atomic<const char *> &gs_vector_alloc_forced_failure_site() {
	static std::atomic<const char *> site{ nullptr };
	return site;
}

inline void gs_vector_alloc_force_failure_at(const char *p_where) {
	gs_vector_alloc_forced_failure_site().store(p_where, std::memory_order_relaxed);
}

inline void gs_vector_alloc_clear_forced_failure() {
	gs_vector_alloc_forced_failure_site().store(nullptr, std::memory_order_relaxed);
}

// True once the armed site has actually fired. A test MUST assert this: it is the only
// thing that keeps the test honest if the production site is later renamed, reordered or
// removed -- otherwise the injection silently no-ops and the case goes vacuous again,
// which is exactly the defect this seam exists to fix.
inline bool gs_vector_alloc_forced_failure_is_armed() {
	return gs_vector_alloc_forced_failure_site().load(std::memory_order_relaxed) != nullptr;
}

inline bool gs_vector_alloc_consume_forced_failure(const char *p_where) {
	const char *armed = gs_vector_alloc_forced_failure_site().load(std::memory_order_relaxed);
	if (armed == nullptr || p_where == nullptr || strcmp(armed, p_where) != 0) {
		return false;
	}
	return gs_vector_alloc_forced_failure_site().compare_exchange_strong(
			armed, nullptr, std::memory_order_relaxed);
}
#endif // TESTS_ENABLED

// Resizes p_vec to p_count. On success returns true. On allocation failure clears
// p_vec, emits an error naming the sizes, and returns false; the caller must then
// apply its OWN failure contract (return false / return an Error / skip the entry).
// It deliberately does not choose one.
template <typename T>
[[nodiscard]] bool gs_resize_or_fail(Vector<T> &p_vec, int64_t p_count, const char *p_where) {
	if (p_count < 0) {
		p_vec.clear();
		ERR_FAIL_V_MSG(false, vformat("%s: refusing to allocate a negative count (%d).",
									  String(p_where), p_count));
	}
#if defined(TESTS_ENABLED)
	// Placed AFTER the negative-count guard and BEFORE the resize so the injected run takes
	// the same exit as a real allocation failure: output cleared, error emitted, false.
	if (gs_vector_alloc_consume_forced_failure(p_where)) {
		p_vec.clear();
		ERR_FAIL_V_MSG(false,
				vformat("%s: injected allocation failure (test seam); "
						"leaving the output empty instead of writing out of bounds.",
						String(p_where)));
	}
#endif
	if (p_vec.resize((typename Vector<T>::Size)p_count) == OK) {
		return true;
	}
	p_vec.clear();
	ERR_FAIL_V_MSG(false,
			vformat("%s: failed to allocate %d entries of %d bytes (%d bytes total); "
					"leaving the output empty instead of writing out of bounds.",
					String(p_where), p_count, (int64_t)sizeof(T),
					p_count * (int64_t)sizeof(T)));
}
