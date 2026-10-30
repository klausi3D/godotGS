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
// Package F1 (ADR 14.5, editable regions): an edit replaces the payload of leaf nodes and makes their ancestors STALE.
//   stage_edit()    admits the whole edit or none of it: a reserved SHADOW run for every edited leaf (the old run keeps
//                   serving the old content, which stays consistent until the flip), one ticket per leaf, and a hold on
//                   the children of every stale node (the replacement cut the selector will draw), which therefore
//                   cannot be evicted. Nothing drawable changes while the edit is only staged.
//   edit_prefetch() the children of stale nodes that still need loading, as ordinary demand.
//   finish_shadow() a packed shadow payload arrived (ticket checked against world, device and edit generation).
//   commit_edit()   the atomic publication boundary: once every shadow is ready and every replacement child is
//                   RESIDENT, each edited leaf takes its shadow run (the old run retires, any in-flight ticket of the
//                   old content dies) and every stale node is dropped (mask 0, run retired, never requested again).
//   cancel_edit()   gives the reservation back; the world is exactly as before the edit was staged.
// One edit is staged at a time and its revision must follow the committed one (expected prior revision).
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
		bool stale = false; // Package F1: ancestor of an edited leaf in an already edited snapshot.
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
		uint64_t committed_edit_revision = 0; // Package F1: the edit revision of the snapshot this table describes.
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
		bool shadow = false; // Package F1: a payload for the edited leaf's reserved shadow run, not for the node's own run.
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
		// Package F1.
		bool edit_staged = false;
		bool edit_waiting_for_space = false; // Admitted, but the shadow runs are not allocated yet (pages retiring or to be freed).
		uint32_t pages_shadow = 0; // Pages reserved for staged edited leaves (also counted in pages_in_flight).
		uint32_t stale_nodes = 0; // Nodes dropped by committed edits.
		uint64_t edits_committed_total = 0;
		uint64_t edits_cancelled_total = 0;
		uint64_t edits_rejected_total = 0;
		uint64_t committed_edit_revision = 0;
	};

	enum EditResult : uint8_t {
		EDIT_OK = 0,
		EDIT_BUSY, // Another edit is staged, or the residency model is not initialized.
		EDIT_INVALID, // Wrong revision, bad node, not a leaf, empty leaf, duplicate, or a leaf that is another's ancestor.
		EDIT_NO_SPACE, // The shadow runs do not fit (after bounded eviction): nothing was changed.
	};
	struct EditedLeaf {
		uint32_t node = 0;
		uint32_t splat_count = 0; // The replacement payload size.
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

	// --- Package F1 ---
	// Admits the whole edit or none of it, WITHOUT evicting anything: the decision is free + retiring + reclaimable
	// (unprotected, unheld) pages against the shadow runs. Tickets are appended when the shadow runs could be allocated
	// at once; otherwise the edit waits for space and advance_edit() delivers them (and evicts, after the frame's
	// protection is known). EDIT_NO_SPACE changes nothing at all.
	EditResult stage_edit(uint64_t p_revision, const EditedLeaf *p_leaves, uint32_t p_count, LocalVector<Ticket> &r_tickets);
	// Call after protect_drawn() for the frame: allocates the waiting shadow runs, evicting unprotected nodes when needed.
	void advance_edit(LocalVector<Ticket> &r_tickets);
	// Frames since the edit was staged (0 when none).
	uint64_t staged_edit_age_frames() const { return edit_staged ? current_frame - staged_frame : 0; }
	bool is_edit_staged() const { return edit_staged; }
	uint64_t staged_edit_revision() const { return edit_staged ? staged_revision : 0; }
	uint64_t committed_edit_revision() const { return committed_revision; }
	// Appends the replacement children that are not resident yet (children of one parent consecutive), as demand for
	// request(). Empty when no edit is staged.
	void edit_prefetch(LocalVector<uint32_t> &r_demand) const;
	// True while the ticket is the current shadow request of its leaf and its payload has not arrived.
	bool is_shadow_pending(const Ticket &p_ticket) const;
	// A packed shadow payload arrived (true) or could not be built (false). False when the ticket is not current.
	bool finish_shadow(const Ticket &p_ticket, bool p_success);
	bool edit_ready() const;
	// A shadow payload failed or a replacement child can never load: the edit cannot commit and should be cancelled.
	bool edit_blocked() const;
	bool commit_edit();
	bool cancel_edit();
	bool is_stale(uint32_t p_node) const { return p_node < uint32_t(nodes.size()) && nodes[p_node].stale; }

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
		// Package F1.
		bool stale = false; // Ancestor of an edited leaf: never requested, never drawn, not protected, not pinned.
		bool edit_hold = false; // Replacement child of a staged edit: protected until the edit commits or is cancelled.
		enum ShadowState : uint8_t { SHADOW_NONE = 0, SHADOW_LOADING, SHADOW_READY, SHADOW_FAILED };
		ShadowState shadow_state = SHADOW_NONE;
		uint64_t shadow_key = 0;
		uint32_t shadow_first = 0;
		uint32_t shadow_pages = 0;
		uint64_t shadow_generation = 0;
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
	// Package F1: the staged edit.
	bool edit_staged = false;
	uint64_t staged_revision = 0;
	uint64_t committed_revision = 0;
	LocalVector<uint32_t> staged_leaves;
	LocalVector<uint32_t> staged_splats; // Replacement payload size per staged leaf.
	LocalVector<uint32_t> staged_stale;
	uint64_t staged_frame = 0;
	bool shadow_alloc_pending = false;
	bool try_allocate_shadows(LocalVector<Ticket> &r_tickets);
	LocalVector<uint32_t> staged_required; // Replacement children, grouped by parent.
	void release_edit_state();

	uint64_t make_alloc_key(uint32_t p_node);
	void retire_run(Node &r_node);
	void release_run_now(Node &r_node);
	bool ticket_current(const Ticket &p_ticket, State p_expected) const;
	// A completion was rejected: counted as harmless when the ticket's request was cancelled in this world and
	// device generation, as real loss otherwise.
	void count_rejected_completion(const Ticket &p_ticket);
	// True once per cancelled ticket: the first late delivery consumes its window entry, so a duplicate delivery
	// of the same ticket is real loss again.
	bool consume_cancelled(const Ticket &p_ticket);
	// Records a request or shadow generation that is being killed, so its late completion is harmless.
	void note_cancelled_generation(uint64_t p_generation);
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
