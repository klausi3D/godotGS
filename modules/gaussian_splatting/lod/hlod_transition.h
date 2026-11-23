#ifndef GS_HLOD_TRANSITION_H
#define GS_HLOD_TRANSITION_H

#include <cstdint>

namespace gs_hlod_runtime {

enum TransitionKind : uint8_t {
	TRANSITION_INFERRED = 0, // Compatibility for callers without selector descriptors.
	TRANSITION_ROOT = 1,
	TRANSITION_REFINEMENT = 2,
	TRANSITION_BOOTSTRAP = 3,
};

// One selector emission. Visible siblings include residents; demand contains only missing nodes.
// Node indices and ordinal belong to this selection of one instance. Selection state is owner-thread only.
struct TransitionGroup {
	uint32_t parent = UINT32_MAX;
	uint32_t first_child = 0;
	uint32_t ordinal = 0;
	uint16_t visible_mask = 0;
	TransitionKind kind = TRANSITION_INFERRED;
	// Service priority of this emission (ADR adr-hlod-streaming.md 6.6), set by ViewSet when it publishes demand: the
	// current projected screen-space error of the parent whose replacement it completes, in pixels; +inf for the root
	// and the bootstrap frontier (nothing draws until they are resident). Larger is served first. 0 for callers without
	// it, which then keep their demand order. Recomputed every selection, so a consumer sees this frame's priority.
	float priority = 0.0f;
};

} // namespace gs_hlod_runtime
#endif
