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
};

} // namespace gs_hlod_runtime
#endif
