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
