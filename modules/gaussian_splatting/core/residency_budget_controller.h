#ifndef RESIDENCY_BUDGET_CONTROLLER_H
#define RESIDENCY_BUDGET_CONTROLLER_H

#include <cstdint>

class ResidencyBudgetController {
public:
    enum class AdmissionDecision : uint8_t {
        LoadDirect = 0,
        EvictThenLoad = 1,
        Skip = 2,
    };

    struct AdmissionFrameBudget {
        uint32_t effective_max = 0;
        uint32_t evictions_left = 0;
        bool eviction_blocked = false;
    };

    struct AdmissionPolicy {
        bool can_replace_without_eviction = false;
        bool enforce_vram_regulator_gate = false;
        bool vram_regulator_allows_load = true;
        bool atlas_slots_full = false;
        // #1176: admission intent. A needed load (visible demand, explicit residency
        // request) may fall back to evicting a visible chunk; a predictive prefetch may
        // not, so an off-screen guess can never open a hole on screen. Only the
        // visible-eviction fallback reads it; the admission decision itself does not.
        bool allow_visible_eviction = true;
    };

    struct AdmissionContext {
        uint32_t loaded_chunks = 0;
        uint32_t effective_max = 0;
        bool can_replace_without_eviction = false;
        bool has_eviction_budget = false;
        bool eviction_blocked = false;
        bool enforce_vram_regulator_gate = false;
        bool vram_regulator_allows_load = true;
        bool atlas_slots_full = false;
        bool allow_visible_eviction = true;
    };

    struct AdmissionGate {
        AdmissionContext context;
        AdmissionDecision decision = AdmissionDecision::Skip;
    };

    // Invariants:
    // - Decision must be deterministic from AdmissionContext.
    // - note_successful_eviction() must not underflow evictions_left.
    static AdmissionFrameBudget make_frame_budget(uint32_t p_effective_max, uint32_t p_evictions_left, bool p_eviction_blocked);
    static AdmissionGate compute_admission_gate(
            uint32_t p_loaded_chunks,
            const AdmissionFrameBudget &p_frame_budget,
            const AdmissionPolicy &p_policy);
    static void note_successful_eviction(AdmissionFrameBudget &p_frame_budget);
    static void note_blocked_eviction(AdmissionFrameBudget &p_frame_budget);
    // Visible-eviction fallback is valid only when admission required eviction due
    // to hard chunk-cap pressure, atlas-slot pressure, or regulator-gated pressure,
    // and only for an admission whose intent allows it (#1176).
    static bool should_attempt_visible_evict_fallback(const AdmissionGate &p_gate);
    // #1176: true when the pressure alone would have taken the visible-eviction fallback
    // but the admission intent (prefetch) forbids it, i.e. the load is skipped instead.
    static bool is_visible_evict_fallback_refused_by_intent(const AdmissionGate &p_gate);
    static AdmissionDecision decide_admission(const AdmissionContext &p_context);
};

#endif // RESIDENCY_BUDGET_CONTROLLER_H
