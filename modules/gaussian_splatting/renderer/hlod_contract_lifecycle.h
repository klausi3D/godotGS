#ifndef GS_HLOD_CONTRACT_LIFECYCLE_H
#define GS_HLOD_CONTRACT_LIFECYCLE_H

#include <cstdint>

namespace gs_hlod_bundle {

enum class ContractChangeAction { ROWS_ONLY, INSTANCE_REGION_ONLY, FULL_REBUILD };
enum class ContractChangeReason { UNCHANGED, ROWS_CHANGED, ASSETS_CHANGED, OBJECT_TAIL_OVERFLOW, WORLD_DEVICE_SOURCE_CHANGED, RESOURCES_CHANGED, INSTANCE_REFRESH_FAILED };

struct ContractChange {
	bool world_device_source_current = true;
	bool resources_current = true;
	uint64_t prepared_asset_generation = 0, asset_generation = 0;
	// Zero means no failed attempt; a different asset generation may try the incremental path again.
	uint64_t instance_refresh_failed_asset_generation = 0;
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
		return { ContractChangeAction::INSTANCE_REGION_ONLY, ContractChangeReason::ASSETS_CHANGED };
	}
	return { ContractChangeAction::ROWS_ONLY, p_change.prepared_row_generation == p_change.row_generation ?
			ContractChangeReason::UNCHANGED : ContractChangeReason::ROWS_CHANGED };
}

// The last step may partially write the retained atlas. A failure must discard
// new tables before tearing down the old owner; it cannot roll back that write.
template <typename Upload, typename Commit, typename Discard, typename Teardown>
bool publish_instance_tail(Upload p_upload, Commit p_commit, Discard p_discard, Teardown p_teardown) {
	if (!p_upload()) {
		p_discard();
		p_teardown();
		return false;
	}
	p_commit();
	return true;
}

inline uint64_t object_tail_pages(uint64_t p_pages, uint32_t p_percent) {
	const uint32_t percent = p_percent > 100 ? 100 : p_percent;
	return (p_pages / 100) * percent + ((p_pages % 100) * percent) / 100;
}

struct ObjectTailPlacement {
	uint64_t world_pages;
	uint64_t tail_pages;
};

// Percentage base: the existing tree-capped world page budget. Unused atlas
// headroom supplies that target first; it never enlarges the percentage target.
inline ObjectTailPlacement place_object_tail(uint64_t p_total_node_pages, uint64_t p_page_budget, uint32_t p_percent) {
	const uint64_t world = p_total_node_pages < p_page_budget ? p_total_node_pages : p_page_budget;
	const uint64_t tail = object_tail_pages(world, p_percent);
	const uint64_t headroom = p_page_budget > p_total_node_pages ? p_page_budget - p_total_node_pages : 0;
	const uint64_t from_headroom = tail < headroom ? tail : headroom;
	return { world - (tail - from_headroom), tail };
}

} // namespace gs_hlod_bundle

#endif // GS_HLOD_CONTRACT_LIFECYCLE_H
