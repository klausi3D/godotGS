#ifndef GS_HLOD_NODE_RESIDENCY_H
#define GS_HLOD_NODE_RESIDENCY_H

// ADR docs/architecture/adr-hlod-streaming.md sections 6.6, 14.1 and 14.2 (package D1).
//
// CPU-only residency model for HLOD tree nodes. It is the node-level front of the streaming system's page
// allocator (GaussianAtlasAllocator, #1088) and follows the same contracts the chunk streamer uses:
// whole-run admission, per-request generations (the #1177 upload-sequence idea: a completion whose ticket is
// not the node's current one is dropped), a retirement delay before a freed run may be reused, and an
// eviction order of outside the protected closure, then least recently drawn, then finest level first.
// It owns no GPU resource and performs no I/O: the renderer glue (renderer/hlod_node_streamer.*) packs and
// uploads, this class decides what is requested, what may be freed and what counts as resident-ready.
//
//   ABSENT --request--> LOADING --begin_upload--> UPLOADING --finish_upload(ok)--> RESIDENT
//                          |                          |  \--finish_upload(fail)--> FAILED
//                          \--fail_load--> FAILED     \--cancel--> ABSENT (run retires, ticket dead)
//
// Only RESIDENT nodes are visible to the selector (resident_mask()). A node is RESIDENT only after its
// payload was validated, completely uploaded and finish_upload() was called for the current ticket on the
// current world and device generation; an allocated or partly uploaded run is accounted for separately.
//
// Threading: not thread-safe; call from the thread that owns the selection state (the render thread).

#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/streaming_atlas.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include <cstdint>

namespace gs_hlod_runtime {

class NodeResidency {
public:
	enum State : uint8_t {
		ABSENT = 0,
		LOADING, // Run allocated, payload being read/packed. Not drawable.
		UPLOADING, // Payload ready, GPU upload in progress. Not drawable.
		RESIDENT, // Validated, completely uploaded, valid GPU lifetime. The only drawable state.
		FAILED, // Read, validation or upload failed; retried after a delay, permanent after max_failures.
	};

	struct NodeInfo {
		uint32_t splat_count = 0;
		uint32_t parent = UINT32_MAX; // UINT32_MAX for the root.
		uint32_t height = 0; // 0 = leaf. Larger = coarser. Used for the finest-first eviction order.
	};

	struct Config {
		uint32_t page_capacity = 0; // Atlas size in pages.
		uint32_t page_splats = 1024;
		// Frames a freed (evicted, cancelled, rolled back) run stays allocated before it can be reused:
		// resources awaiting GPU retirement are still protected (ADR 14.1).
		uint32_t retire_delay_frames = 3;
		uint32_t max_requests_per_frame = 64; // Nodes newly requested per begin_frame().
		uint32_t evictions_per_frame = 32;
		// A LOADING request that was not demanded for this many frames is cancelled.
		uint32_t cancel_grace_frames = 4;
		uint32_t retry_delay_frames = 8;
		uint32_t max_failures = 3;
		bool pin_root = true; // The root is the complete coarse fallback; it is never evicted.
	};

	// A request handle. Every field is checked on completion: the world generation (which world/snapshot the
	// request was made for), the device generation (which RenderingDevice the run lives on) and the request
	// generation (which request of this node, so a cancelled or overwritten request cannot publish).
	struct Ticket {
		uint32_t node = UINT32_MAX;
		uint32_t first_page = 0;
		uint32_t page_count = 0;
		uint64_t request_generation = 0;
		uint64_t world_generation = 0;
		uint64_t device_generation = 0;
		bool valid() const { return node != UINT32_MAX && request_generation != 0; }
	};

	struct Stats {
		uint32_t absent = 0, loading = 0, uploading = 0, resident = 0, failed = 0, permanently_failed = 0;
		uint32_t pages_resident = 0; // Pages of RESIDENT nodes.
		uint32_t pages_in_flight = 0; // Pages of LOADING and UPLOADING nodes.
		uint32_t pages_retiring = 0; // Freed runs still awaiting retirement.
		uint32_t pages_free = 0; // Allocator free pages (retiring runs are not free).
		uint32_t largest_free_run = 0;
		uint64_t requested_total = 0;
		uint64_t completed_total = 0;
		uint64_t cancelled_total = 0;
		// Real loss: a completion rejected by a generation or state check although its request was never cancelled
		// (stale world or device generation, a duplicate or out-of-order completion). The zero rule applies here.
		uint64_t late_dropped_total = 0;
		// Harmless: the completion of a request that cancel() had already killed. The node was released by the
		// cancel, so nothing was drawn wrong and nothing was lost. Never part of the zero rule.
		uint64_t late_dropped_cancelled_total = 0;
		uint64_t evicted_total = 0;
		uint64_t failed_total = 0;
		uint64_t skipped_groups_total = 0; // Whole transitions that did not fit and were skipped.
	};

	struct RequestResult {
		uint32_t requested = 0; // Tickets appended.
		uint32_t satisfied = 0; // Demanded nodes already LOADING/UPLOADING/RESIDENT.
		uint32_t skipped_groups = 0; // Whole transitions that did not fit this frame.
		uint32_t evicted = 0;
		bool capped = false; // max_requests_per_frame reached: remaining demand waits.
		// Why groups did not progress, kept apart (ADR 14.2: failed is not a budget problem):
		uint32_t blocked_groups = 0; // A child failed (waiting for retry or permanently): the transition cannot complete.
		uint32_t fragmented_groups = 0; // Enough pages are free in total but no contiguous run fits (compaction's job, D2).
		uint32_t pending_retire_groups = 0; // Skipped only because the needed pages are still retiring.
	};

	// Sizes every table and the allocator. Fails (false) on an empty or inconsistent table. Resets all state;
	// call again with new generations for a new world or a new device.
	bool reset(const Vector<NodeInfo> &p_nodes, const Config &p_config, uint64_t p_world_generation, uint64_t p_device_generation);
	bool is_initialized() const { return !nodes.is_empty(); }

	// Starts a frame: retires freed runs whose delay elapsed and clears the per-frame protection.
	void begin_frame(uint64_t p_frame);
	uint64_t frame() const { return current_frame; }

	// Protection (ADR 14.1): union over admitted views of drawn versions and their fallback coverage.
	// Marks each node and all of its ancestors, and stamps them as drawn this frame. Call once per admitted
	// view per frame, after its selection.
	void protect_drawn(const uint32_t *p_nodes, uint32_t p_count);
	bool is_protected(uint32_t p_node) const;

	// Demand (Desired(view)) in priority order, children of one parent consecutive. Each parent's missing
	// children are admitted as one whole transition: every run is allocated or none is. A group that does not
	// fit (after bounded eviction outside the protected closure) is skipped, not waited for. Appends tickets
	// for the nodes that now need a payload. Also cancels LOADING requests that stayed undemanded past the
	// grace period.
	RequestResult request(const uint32_t *p_demand, uint32_t p_count, LocalVector<Ticket> &r_tickets);

	// True while the ticket is the node's current request and its payload has not been handed to the upload
	// yet (LOADING). Does not count as a dropped completion; used to skip work for cancelled requests.
	bool is_pending(const Ticket &p_ticket) const { return ticket_current(p_ticket, LOADING); }
	// Worker finished reading/packing a payload. True when the ticket is current and the node moved to
	// UPLOADING; false when the world, device or request generation or the node state no longer
	// matches. The rejection counts in late_dropped_cancelled_total when the request had been cancelled, and in
	// late_dropped_total (real loss) otherwise. A rejected ticket never touches the node's current state.
	bool begin_upload(const Ticket &p_ticket);
	// The upload command stream for the whole run was issued (true) or failed (false). Only a current
	// UPLOADING ticket can make a node RESIDENT.
	bool finish_upload(const Ticket &p_ticket, bool p_success);
	// The worker could not read or validate the payload.
	bool fail_load(const Ticket &p_ticket);
	// Drops an in-flight request (LOADING or UPLOADING). Its run is not reusable until the retirement delay
	// elapsed and its ticket is dead. Returns false for a node that is not in flight.
	bool cancel(uint32_t p_node);
	// Evicts one RESIDENT node (never a protected one). The run retires; returns false when refused.
	bool evict(uint32_t p_node);

	// Nodes evicted since the last call (the glue clears their GPU node metadata, so an evicted node can never
	// be drawn from a run that is about to be reused).
	void take_evicted(LocalVector<uint32_t> &r_nodes) {
		r_nodes = evicted_log;
		evicted_log.clear();
	}

	State state(uint32_t p_node) const { return p_node < uint32_t(nodes.size()) ? nodes[p_node].state : ABSENT; }
	bool run(uint32_t p_node, uint32_t &r_first_page, uint32_t &r_page_count) const;
	uint32_t pages_for(uint32_t p_node) const { return p_node < uint32_t(nodes.size()) ? nodes[p_node].pages : 0; }
	// 1 per node = RESIDENT. Copy-on-write shared with the selector's snapshot.
	const Vector<uint8_t> &resident_mask() const { return mask; }
	Stats stats() const;
	// True when the root can never become resident (permanent failure): the view is FAILED, not LOADING.
	bool root_permanently_failed() const;
	uint32_t node_count() const { return nodes.size(); }
	uint64_t world_generation() const { return world_gen; }
	uint64_t device_generation() const { return device_gen; }
	const Config &config() const { return cfg; }

private:
	struct Node {
		State state = ABSENT;
		uint32_t parent = UINT32_MAX;
		uint32_t height = 0;
		uint32_t pages = 0;
		uint32_t first_page = 0;
		uint64_t alloc_key = 0; // Unique per allocation, so a retiring run is never confused with a new one.
		uint64_t request_generation = 0;
		uint64_t last_drawn_frame = 0;
		uint64_t last_demand_frame = 0;
		uint64_t protected_frame = UINT64_MAX; // == current_frame while protected.
		uint64_t retry_after_frame = 0;
		uint64_t demand_pass = 0; // request() pass that last listed this node (duplicate demand is one demand).
		uint32_t failures = 0;
	};
	struct Retiring {
		uint64_t alloc_key = 0;
		uint32_t first_page = 0;
		uint32_t pages = 0;
		uint64_t retire_frame = 0;
	};

	Vector<Node> nodes;
	Vector<uint8_t> mask;
	Config cfg;
	GaussianAtlasAllocator allocator;
	LocalVector<Retiring> retiring;
	LocalVector<uint32_t> candidates; // Eviction scratch.
	LocalVector<uint32_t> evicted_log;
	LocalVector<uint32_t> demand_unique; // request() scratch: the demand without duplicates, order kept.
	uint64_t world_gen = 0, device_gen = 0;
	uint64_t current_frame = 0;
	uint64_t request_serial = 0, alloc_serial = 0, dedup_pass = 0;
	uint32_t requests_this_frame = 0;
	uint32_t evictions_this_frame = 0;
	uint32_t pages_retiring = 0;
	Stats totals;

	uint64_t make_alloc_key(uint32_t p_node);
	void retire_run(Node &r_node);
	void release_run_now(Node &r_node);
	bool ticket_current(const Ticket &p_ticket, State p_expected) const;
	// A completion was rejected: counted as harmless when the ticket's request was cancelled in this world and
	// device generation, as real loss otherwise.
	void count_rejected_completion(const Ticket &p_ticket);
	bool was_cancelled(const Ticket &p_ticket) const;
	// Request generations killed by cancel(), newest overwriting oldest. Request generations are unique per
	// reset(), so membership identifies the request. A ticket older than the window is counted as real loss
	// (fail closed). Cleared by reset().
	static constexpr uint32_t CANCELLED_WINDOW = 256;
	LocalVector<uint64_t> cancelled_generations;
	uint32_t cancelled_next = 0;
	// Evicts unprotected RESIDENT nodes (LRU, then finest level, then node index) until `p_pages` pages are
	// free or pending retirement. Returns the number of evictions; bounded by evictions_per_frame.
	uint32_t evict_for(uint32_t p_pages, uint32_t p_exclude_parent);
	// Bounded answer to fragmentation (compaction itself is package D2): when enough pages are free in total but
	// no contiguous run of `p_pages` exists, evicts the cheapest set of unprotected RESIDENT nodes whose runs,
	// together with free pages, form one contiguous window. Windows crossing a protected, in-flight or retiring
	// run are not considered, so protection and GPU lifetime rules still hold. Returns the number of evictions.
	uint32_t evict_window(uint32_t p_pages, uint32_t p_exclude_parent);
	LocalVector<uint32_t> window_owner; // evict_window() scratch: node index, FREE_PAGE or BLOCKED_PAGE per page.
};

// Builds the residency table input from the runtime data (node payload counts, parents, heights).
bool make_node_infos(const GaussianSplatHlodRuntimeData &p_data, Vector<NodeResidency::NodeInfo> &r_infos);

} // namespace gs_hlod_runtime

#endif
