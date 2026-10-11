#pragma once

#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)

#include "core/error/error_macros.h"
#include "core/core_globals.h"
#include "core/os/thread.h"

#include <atomic>
#include <cstdint>
#include <cstring>

// Explicit live-test context only. No shipping state, ownership or scheduling.
namespace GSLiveEditorObservation {
enum Kind { ERROR_EVENT = 1, ARM, CONSUME, GENERATE_BEGIN, GENERATE_END,
	PREVIEW_SCHEDULE, PREVIEW_ENTRY, PREVIEW_COMPLETE, PREVIEW_JOIN, LOGGER_MISMATCH };
struct Record {
	std::atomic<bool> ready{ false };
	uint32_t sequence = 0, kind = 0, case_id = 0, phase = 0;
	uint64_t thread = 0, generator = 0, asset = 0, token = 0, arm = 0;
	int64_t value = 0;
	int line = 0;
	bool expected = false, notify = false;
	char function[128]{}, file[512]{}, error[384]{}, message[768]{};
};
struct Observer {
	static constexpr uint32_t CAPACITY = 4096;
	std::atomic<uint32_t> next{ 0 }, current_case{ 0 }, arms{ 0 }, invocations{ 0 };
	std::atomic<uint64_t> preview_generator{ 0 };
	std::atomic<bool> invalid{ false }, thumbnail_case{ false }, preview_case{ false };
	uint64_t owner = 0;
	Record records[CAPACITY];
};
// The core error handler executes under _global_lock. A locking atomic
// implementation would violate its no-second-lock contract: refuse that target
// at compile time, before any observer can be published or handler installed.
static_assert(std::atomic<bool>::is_always_lock_free, "Live editor observer requires lock-free bool atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Live editor observer requires lock-free u32 atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "Live editor observer requires lock-free u64 atomics");
static_assert(std::atomic<Observer *>::is_always_lock_free, "Live editor observer requires lock-free pointer atomics");
inline std::atomic<Observer *> active{ nullptr };
// Increment BEFORE loading active. After disarm, zero readers proves no helper
// can still dereference userdata; later readers see null. Core error callbacks
// have their separate existing remove_error_handler/global-lock closure.
inline std::atomic<uint32_t> helper_readers{ 0 };
inline std::atomic<bool> helper_overflow{ false };
class Access {
	Observer *observer = nullptr;
	bool held = false;
public:
	Access() {
		uint32_t n = helper_readers.load(std::memory_order_seq_cst);
		while (n != UINT32_MAX && !helper_readers.compare_exchange_weak(n, n + 1, std::memory_order_seq_cst)) {}
		if (n == UINT32_MAX) { helper_overflow.store(true, std::memory_order_seq_cst); return; }
		held = true;
		observer = active.load(std::memory_order_seq_cst);
		if (!observer) { helper_readers.fetch_sub(1, std::memory_order_seq_cst); held = false; }
	}
	Observer *get() const { return observer; }
	~Access() { if (held) { helper_readers.fetch_sub(1, std::memory_order_seq_cst); } }
	Access(const Access &) = delete;
	Access &operator=(const Access &) = delete;
};
struct Phase {
	uint32_t ordinal = 0, case_id = 0, arm = 0;
	uint64_t generator = 0, asset = 0, invocation = 0;
	int size = 0, style = -1, splats = 0;
};
struct Emission {
	bool active = false, claimed = false;
	uint32_t case_id = 0, phase = 0, arm = 0;
	uint64_t invocation = 0, generator = 0, asset = 0;
	const char *function = nullptr, *file = nullptr;
	int line = 0;
};
inline thread_local Phase phase;
inline thread_local Emission emission;
inline thread_local Phase consumed_phase;
inline constexpr const char *HITS = "GaussianThumbnailGenerator::_project_to_canvas hits";
inline constexpr const char *RATIONALE = "GaussianThumbnailGenerator::_project_to_canvas hits: injected allocation failure (test seam); leaving the output empty instead of writing out of bounds.";

inline Record *reserve(Observer *o, Kind kind) {
	uint32_t n = o->next.load(std::memory_order_relaxed);
	while (n < Observer::CAPACITY && !o->next.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) {}
	if (n >= Observer::CAPACITY) {
		o->invalid.store(true, std::memory_order_release);
		return nullptr;
	}
	Record *r = &o->records[n];
	r->sequence = n + 1;
	r->kind = kind;
	r->case_id = o->current_case.load(std::memory_order_acquire);
	r->thread = Thread::get_caller_id();
	return r;
}
inline void publish(Record *r) { r->ready.store(true, std::memory_order_release); }

template <size_t N>
inline bool copy(char (&dst)[N], const char *src) {
	if (!src) {
		dst[0] = '\0';
		return false;
	}
	for (size_t i = 0; i < N; ++i) {
		dst[i] = src[i];
		if (!src[i]) {
			return true;
		}
	}
	dst[N - 1] = '\0';
	return false;
}

inline uint32_t advance_two(Observer *o, std::atomic<uint32_t> &counter) {
	uint32_t n = counter.load(std::memory_order_acquire);
	while (n < 2 && !counter.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel)) {}
	if (n >= 2) { o->invalid.store(true, std::memory_order_release); return 0; }
	return n + 1;
}
inline void observe_arm(const char *where) {
	Access access;
	Observer *o = access.get();
	if (!o) { return; }
	if (!o->thumbnail_case.load(std::memory_order_acquire) || Thread::get_caller_id() != o->owner || !where || strcmp(where, HITS)) {
		o->invalid.store(true, std::memory_order_release);
	}
	uint32_t ordinal = advance_two(o, o->arms);
	if (ordinal > 2) { o->invalid.store(true, std::memory_order_release); }
	Record *r = reserve(o, ARM);
	if (r) {
		r->arm = ordinal;
		if (!copy(r->message, where)) { o->invalid.store(true, std::memory_order_release); }
		publish(r);
	}
}
inline void observe_consume(const char *where) {
	Access access;
	Observer *o = access.get();
	if (!o) { return; }
	Record *r = reserve(o, CONSUME);
	if (r) {
		r->phase = phase.ordinal; r->arm = o->arms.load(std::memory_order_acquire);
		r->token = phase.invocation; r->generator = phase.generator; r->asset = phase.asset;
		if (!copy(r->message, where)) { o->invalid.store(true, std::memory_order_release); }
		if (!phase.ordinal || !phase.invocation || phase.arm != r->arm || r->thread != o->owner || !where || strcmp(where, HITS) || consumed_phase.invocation) {
			o->invalid.store(true, std::memory_order_release);
		} else { consumed_phase = phase; }
		publish(r);
	}
}
class PhaseScope {
	Access access;
	bool engaged = false;
public:
	PhaseScope(uint32_t ordinal, uint64_t generator, uint64_t asset, int size, int style, int splats) {
		Observer *o = access.get();
		if (!o) { return; }
		if (Thread::get_caller_id() != o->owner || phase.ordinal || !o->thumbnail_case.load(std::memory_order_acquire) || !generator || !asset || size != 16 || style != 0 || splats != 8 || ordinal < 1 || ordinal > 2 || o->arms.load() != ordinal) {
			o->invalid.store(true, std::memory_order_release);
			return;
		}
		phase = { ordinal, o->current_case.load(), ordinal, generator, asset, 0, size, style, splats };
		engaged = true;
	}
	bool valid() const { return !access.get() || engaged; }
	~PhaseScope() {
		if (engaged) {
			Observer *o = access.get();
			if (!CoreGlobals::print_error_enabled) {
				o->invalid.store(true, std::memory_order_release);
				Record *r = reserve(o, LOGGER_MISMATCH);
				if (r) { r->phase = phase.ordinal; publish(r); }
				// Owner-only repair before outer fixture/resource destructors.
				CoreGlobals::print_error_enabled = true;
			}
			phase = Phase(); consumed_phase = Phase();
		}
	}
};
class GenerateScope {
	Access access;
	Observer *observer = nullptr;
public:
	GenerateScope(uint64_t generator, uint64_t asset, int size, int style) {
		Observer *o = access.get();
		if (!o || !phase.ordinal) { return; }
		observer = o;
		if (Thread::get_caller_id() != o->owner || phase.generator != generator || phase.asset != asset || phase.size != size || phase.style != style || phase.invocation) {
			o->invalid.store(true, std::memory_order_release);
		}
		phase.invocation = advance_two(o, o->invocations);
		if (phase.invocation > 2) { o->invalid.store(true, std::memory_order_release); }
		Record *r = reserve(o, GENERATE_BEGIN);
		if (r) {
			r->phase = phase.ordinal; r->arm = phase.arm; r->token = phase.invocation;
			r->generator = generator; r->asset = asset; r->value = size;
			publish(r);
		}
	}
	~GenerateScope() {
		if (observer) {
			Record *r = reserve(observer, GENERATE_END);
			if (r) {
				r->phase = phase.ordinal; r->arm = phase.arm; r->token = phase.invocation;
				r->generator = phase.generator; r->asset = phase.asset;
				publish(r);
			}
		}
	}
};
class EmissionScope {
	Access access;
	Observer *observer = nullptr;
public:
	EmissionScope(const char *function, const char *file, int line, int64_t count, size_t element_size) {
		Observer *o = access.get();
		if (!o) { return; }
		observer = o;
		if (emission.active || Thread::get_caller_id() != o->owner || !phase.invocation || consumed_phase.invocation != phase.invocation || consumed_phase.case_id != phase.case_id || consumed_phase.arm != phase.arm || consumed_phase.generator != phase.generator || consumed_phase.asset != phase.asset || count != 256 || element_size != sizeof(int)) {
			o->invalid.store(true, std::memory_order_release);
			return;
		}
		emission = { true, false, phase.case_id, phase.ordinal, phase.arm, phase.invocation, phase.generator, phase.asset, function, file, line };
	}
	~EmissionScope() {
		if (observer) {
			if (!emission.active || !emission.claimed) { observer->invalid.store(true, std::memory_order_release); }
			emission = Emission(); consumed_phase = Phase();
		}
	}
};
// Both source coordinates expand at the SAME original error invocation.
#define GS_LIVE_EDITOR_INJECTED_ERROR(m_ret, m_msg, m_count, m_element_size) \
	do { \
		GSLiveEditorObservation::EmissionScope gs_emission_scope(FUNCTION_STR, __FILE__, __LINE__, m_count, m_element_size); \
		ERR_FAIL_V_MSG(m_ret, m_msg); \
	} while (false)

inline void observe_preview_generator(uint64_t generator) {
	Access access;
	Observer *o = access.get();
	if (!o || !o->preview_case.load(std::memory_order_acquire) || Thread::get_caller_id() != o->owner) { return; }
	uint64_t zero = 0;
	if (!o->preview_generator.compare_exchange_strong(zero, generator) || !generator) { o->invalid.store(true, std::memory_order_release); }
}
inline uint32_t preview_case_for(uint64_t generator) {
	Access access;
	Observer *o = access.get();
	return o && o->preview_case.load(std::memory_order_acquire) && o->preview_generator.load() == generator ? o->current_case.load() : 0;
}
inline void preview_event(Kind kind, uint32_t case_id, uint64_t generator, uint64_t request, int64_t value = 0) {
	Access access;
	Observer *o = access.get();
	if (!o || !case_id) { return; }
	Record *r = reserve(o, kind);
	if (r) {
		r->case_id = case_id; r->generator = generator; r->token = request; r->value = value;
		if (!generator || !request || !r->thread) { o->invalid.store(true, std::memory_order_release); }
		publish(r);
	}
}
inline void preview_join(uint64_t worker) {
	Access access;
	Observer *o = access.get();
	if (!o || !o->preview_case.load(std::memory_order_acquire)) { return; }
	Record *r = reserve(o, PREVIEW_JOIN);
	if (r) {
		r->token = worker; r->generator = o->preview_generator.load();
		if (r->thread != o->owner || !worker || worker == o->owner) { o->invalid.store(true, std::memory_order_release); }
		publish(r);
	}
}
} // namespace GSLiveEditorObservation
#endif
