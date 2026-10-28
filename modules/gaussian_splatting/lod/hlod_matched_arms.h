#pragma once

#ifdef TESTS_ENABLED

// Test-only S1b matched-arm quality exporter (ADR docs/architecture/adr-hlod-streaming.md §9 S1b,
// §14.7, §15). NOT a runtime or publication API and not shipped: CPU-only, main thread.
//
// Given one quality bake with an attached gs_hlod::CandidateCapture and ONE explicit complete cut,
// it assembles four splat sets over the same domain:
//   SAT       the MERGED candidate of every cut node,
//   A-area    the SELECTED candidate of every cut node,
//   S1b       the winner the baker kept at every cut node,
//   reference the independent all-leaf set rebuilt from the bake INPUT,
// and writes each through the existing resident-uncompressed .gsplatworld saver with a strict
// native round trip. Nothing is pruned, cropped or filled to reach a count: any mismatch fails with
// a typed status.
//
// Matching contract. At each interior cut node the baker builds SAT and A-area from the same
// children (the baker's chosen child payloads, i.e. the S1b-chosen subtree below) with the same
// candidate count, so the three arms have the same count per node and in total by construction;
// this is verified, never assumed. The cut leaves are original splats in all three arms.

#include "../core/gaussian_splat_hlod_bake.h"
#include "../core/gaussian_splat_world.h"

namespace gs_hlod::matched_arms {

// Predeclared, explicit resource contract. The runtime-cut oracle's own limits
// (gs_hlod::runtime_cut::MAX_*) are NOT changed; this is a separate opt-in contract.
struct Admission {
	uint64_t max_total_splats = 0; // leaves + interior payload splats of the bake
	uint32_t max_nodes = 0;
	// Cap on capture + arms + reference bytes (payload + SH + 1 KiB/node), checked against the actual
	// cut before any arm is allocated. Physical-memory admission is a separate, caller-side check.
	uint64_t max_bytes = 0;

	// Mirrors gs_hlod::runtime_cut::MAX_INPUT_SPLATS / MAX_NODES / MAX_ALLOCATION_BYTES.
	static Admission default_contract();
	// 8,000,000 leaves + interiors, 262,144 nodes, 12 GiB.
	static Admission large_contract();
};

enum class Status {
	OK,
	INVALID_INPUT,
	ADMISSION_REFUSED,
	INVALID_CUT,
	CAPTURE_MISMATCH, // capture does not describe this bake (winner bytes/kind differ from the tree)
	COUNT_MISMATCH, // arms are not equal-count
	REFERENCE_MISMATCH,
	ALLOCATION_FAILED,
	SERIALIZATION_FAILED,
	ROUNDTRIP_MISMATCH,
};
const char *status_name(Status p_status);

// Counts-only admission (no allocation): refuses over the contract.
Status admit(const Admission &p_contract, uint64_t p_leaf_splats, uint64_t p_interior_splats, uint64_t p_nodes,
		uint32_t p_sh_high_order_count, uint64_t p_cut_splats, uint64_t p_capture_bytes, String &r_reason);

struct View {
	Vector3 position;
	Vector3 forward = Vector3(0, 0, -1);
	double focal_pixels = 1000;
	double tau_pixels = 1;
	double near_plane = 0.1;
};
// Top-down complete cut with the runtime-cut oracle's pixel heuristic
// (geometric_error * focal / nearest-AABB-depth <= tau, near/behind bounds refine; leaves stop).
// No budgets: the cut is whatever the heuristic selects. Deterministic, DFS order.
Status select_cut(const GaussianSplatHlodTree &p_tree, const View &p_view, LocalVector<uint32_t> &r_cut, String &r_reason);
// Explicit cut: topmost nodes whose height is <= p_height (leaves have height 0). Always complete.
Status cut_by_height(const GaussianSplatHlodTree &p_tree, uint32_t p_height, LocalVector<uint32_t> &r_cut, String &r_reason);

struct CutNodeRecord {
	uint32_t node = 0;
	bool is_leaf = false;
	bool use_merged = false; // winner at this node (interior only)
	uint32_t candidate_count = 0; // identical for SAT, A-area and S1b
	uint64_t leaf_splat_first = 0; // descendant leaf splat range [first, first + count) in leaf payload order
	uint64_t leaf_splat_count = 0;
	uint32_t leaf_node_first = 0; // range into DomainReport::leaf_node_ids
	uint32_t leaf_node_count = 0;
};
struct DomainReport {
	LocalVector<CutNodeRecord> nodes; // sorted by descendant leaf range (DFS order of the cut)
	LocalVector<uint32_t> leaf_node_ids; // exact descendant leaves, DFS order, concatenated by cut node
	uint64_t domain_splats = 0; // == original splat count; the cut tiles it exactly
	uint64_t cut_splats = 0; // candidate count of each reduced arm
	uint64_t kinds[3] = {}; // cut nodes: leaf, merged winner, selected winner
};

struct ArmPayload {
	LocalVector<Gaussian> gaussians; // absolute positions
	LocalVector<Vector3> sh_high_order;
};
struct ArmMeta {
	uint32_t sh_first_order_count = 0;
	uint32_t sh_high_order_count = 0;
	bool is_2d = false;
	bool antialiased = false; // .gsplatworld has no AA flag: write_arm refuses true instead of dropping it
	Dictionary metadata;
};
struct ArmSet {
	ArmPayload sat, area, s1b, reference;
	DomainReport domain;
};

// p_input is the bake's input (used for the independent reference). p_cut may be in any order.
Status assemble(const BakeInput &p_input, const BakeResult &p_bake, const CandidateCapture &p_capture,
		const LocalVector<uint32_t> &p_cut, const Admission &p_contract, ArmSet &r_arms, String &r_reason);

// Existing serialization route: GaussianData payload + save_resident_uncompressed.
Status write_arm(const ArmPayload &p_arm, const ArmMeta &p_meta, const String &p_path, String &r_reason);
// Reloads p_path and requires bit-exact payload, SH degree/counts, 2D and AA flags, DC tags, metadata.
Status verify_roundtrip(const ArmPayload &p_arm, const ArmMeta &p_meta, const String &p_path, String &r_reason);

} // namespace gs_hlod::matched_arms

#endif
