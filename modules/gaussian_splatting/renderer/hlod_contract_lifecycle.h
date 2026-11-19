#ifndef GS_HLOD_CONTRACT_LIFECYCLE_H
#define GS_HLOD_CONTRACT_LIFECYCLE_H

#include <cstdint>

namespace gs_hlod_bundle {

enum class ContractChangeAction { ROWS_ONLY, INSTANCE_REGION_ONLY, FULL_REBUILD };
enum class ContractChangeReason { UNCHANGED, ROWS_CHANGED, ASSETS_CHANGED, OBJECT_TAIL_OVERFLOW, WORLD_DEVICE_SOURCE_CHANGED, RESOURCES_CHANGED };

struct ContractChange {
	bool world_device_source_current = true;
	bool resources_current = true;
	uint64_t prepared_asset_generation = 0, asset_generation = 0;
	uint64_t prepared_row_generation = 0, row_generation = 0;
	uint64_t object_pages = 0, reserved_object_pages = 0;
};

struct ContractChangeDecision {
	ContractChangeAction action;
	ContractChangeReason reason;
};

// Device-free lifecycle policy, consumed by BOTH publisher stale-contract paths.
// ROWS_ONLY + UNCHANGED needs no upload. Reservation is an input here; allocation,
// project settings and byte accounting remain the resource owner's responsibility.
inline ContractChangeDecision hlod_contract_change_action(const ContractChange &p_change) {
	if (!p_change.world_device_source_current) {
		return { ContractChangeAction::FULL_REBUILD, ContractChangeReason::WORLD_DEVICE_SOURCE_CHANGED };
	}
	if (!p_change.resources_current) {
		return { ContractChangeAction::FULL_REBUILD, ContractChangeReason::RESOURCES_CHANGED };
	}
	if (p_change.prepared_asset_generation != p_change.asset_generation) {
		if (p_change.object_pages > p_change.reserved_object_pages) {
			return { ContractChangeAction::FULL_REBUILD, ContractChangeReason::OBJECT_TAIL_OVERFLOW };
		}
		// #1473 phase A: preserve today's full teardown even when a tail would fit.
		return { ContractChangeAction::FULL_REBUILD, ContractChangeReason::ASSETS_CHANGED };
	}
	return { ContractChangeAction::ROWS_ONLY, p_change.prepared_row_generation == p_change.row_generation ?
			ContractChangeReason::UNCHANGED : ContractChangeReason::ROWS_CHANGED };
}

} // namespace gs_hlod_bundle

#endif // GS_HLOD_CONTRACT_LIFECYCLE_H
