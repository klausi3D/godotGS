#pragma once

// HLOD S1b matched-arm quality exporter (test-only; ADR docs/architecture/adr-hlod-streaming.md §9 S1b,
// §14.7, §15). CPU only. Covers the candidate-capture seam in the baker, matched-arm assembly from
// one common cut, the bounded admission contract, the native round trip and the env-driven driver.
//
// Driver (NOT_RUN unless GS_HLOD_QEXPORT=1; never run on real scans by this change):
//   GS_HLOD_QEXPORT=1 GS_HLOD_QEXPORT_WORLD=<world.gsplatworld> GS_HLOD_QEXPORT_VIEWS=<views.json> \
//   GS_HLOD_QEXPORT_OUTPUT=<fresh dir> [GS_HLOD_QEXPORT_CONTRACT=default|large] [GS_HLOD_QEXPORT_ASSET=<asset>] \
//   <editor binary, TESTS_ENABLED> --headless --test --test-case="*[HLODQualityExport]*" --no-colors

#include "test_macros.h"
#include "test_gaussian_splat_hlod.h"

#include "../core/gaussian_splat_hlod_bake.h"
#include "../core/gaussian_splat_hlod_quality.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../lod/hlod_matched_arms.h"
#include "../lod/hlod_runtime_cut.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

#include <cmath>
#include <cstring>

namespace TestHlodMatchedArms {

using namespace gs_hlod;
using namespace gs_hlod::matched_arms;

struct Fixture {
	LocalVector<Gaussian> g;
	LocalVector<Vector3> sh;
	BakeInput input;
	ArmMeta meta;
};

// 24 flat + 8 volumetric splats (or the alpha control: one visible source among transparent ones, which
// makes SELECTED exact where SAT changes alpha), mixed DC encodings, full compact SH.
inline void make_fixture(Fixture &r_f, uint32_t p_first, uint32_t p_high, bool p_alpha_control) {
	if (p_alpha_control) {
		r_f.g.resize(64u);
		for (uint32_t i = 0; i < r_f.g.size(); ++i) {
			Gaussian g;
			g.position = Vector3(1, 1, 1);
			g.scale = Vector3(0.25f, 0.125f, 0.0625f);
			g.rotation = Quaternion();
			g.opacity = i == 0u ? 0.5f : 0.0f;
			g.sh_dc = Color(0.0f, -0.25f, 0.25f, 1.0f);
			g.painterly_meta = i;
			r_f.g[i] = g;
		}
	} else {
		TestGaussianSplatHlod::hlod_make_fixture(24u, 8u, r_f.g);
	}
	r_f.sh.resize(uint64_t(r_f.g.size()) * p_high);
	for (uint32_t i = 0; i < r_f.g.size(); ++i) {
		r_f.g[i].render_meta = gaussian_set_dc_encoding(
				0u, (i & 1u) ? GAUSSIAN_DC_ENCODING_LEGACY_BIAS : GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		if (p_first) {
			for (int c = 0; c < 3; ++c) {
				r_f.g[i].sh_1[c] = Vector3(0.03125f, -0.015625f, 0.0078125f) * real_t((i + c) % 5u);
			}
		}
		for (uint32_t c = 0; c < p_high; ++c) {
			r_f.sh[uint64_t(i) * p_high + c] = Vector3(0.00390625f, 0.001953125f, -0.0009765625f) * real_t((i + c) % 7u);
		}
	}
	r_f.input = BakeInput();
	r_f.input.gaussians = r_f.g.ptr();
	r_f.input.count = r_f.g.size();
	r_f.input.sh_high_order = p_high ? r_f.sh.ptr() : nullptr;
	r_f.input.sh_first_order_count = p_first;
	r_f.input.sh_high_order_count = p_high;
	r_f.meta = ArmMeta();
	r_f.meta.sh_first_order_count = p_first;
	r_f.meta.sh_high_order_count = p_high;
	r_f.meta.is_2d = false;
	r_f.meta.antialiased = false; // the world format has no AA flag; write_arm refuses AA=true
	r_f.meta.metadata["source"] = "matched-arm-fixture";
	r_f.meta.metadata["degree_high"] = int(p_high);
}

inline BakeParams quality_params(CandidateCapture *p_capture) {
	BakeParams params;
	params.mode = BakeParams::QUALITY_SELECTION;
	params.leaf_max_splats = 4u;
	params.capture = p_capture;
	return params;
}

inline bool same_bytes(const BakeResult &p_a, const BakeResult &p_b) {
	const GaussianSplatHlodTree &a = p_a.tree;
	const GaussianSplatHlodTree &b = p_b.tree;
	if (a.nodes.size() != b.nodes.size() || a.leaf_splat_count != b.leaf_splat_count ||
			a.interior_splat_count != b.interior_splat_count || a.sh_high_order_count != b.sh_high_order_count ||
			a.bake_rule_version != b.bake_rule_version || a.interior_gaussians.size() != b.interior_gaussians.size() ||
			a.interior_sh_high_order.size() != b.interior_sh_high_order.size() ||
			p_a.leaf_gaussians.size() != p_b.leaf_gaussians.size() ||
			p_a.leaf_sh_high_order.size() != p_b.leaf_sh_high_order.size() ||
			p_a.leaf_source_index.size() != p_b.leaf_source_index.size()) {
		return false;
	}
	for (uint32_t i = 0; i < a.nodes.size(); ++i) {
		if (!TestGaussianSplatHlod::hlod_nodes_equal(a.nodes[i], b.nodes[i])) {
			return false;
		}
	}
	return memcmp(a.interior_gaussians.ptr(), b.interior_gaussians.ptr(), sizeof(Gaussian) * a.interior_gaussians.size()) == 0 &&
			memcmp(a.interior_sh_high_order.ptr(), b.interior_sh_high_order.ptr(), sizeof(Vector3) * a.interior_sh_high_order.size()) == 0 &&
			memcmp(p_a.leaf_gaussians.ptr(), p_b.leaf_gaussians.ptr(), sizeof(Gaussian) * p_a.leaf_gaussians.size()) == 0 &&
			memcmp(p_a.leaf_sh_high_order.ptr(), p_b.leaf_sh_high_order.ptr(), sizeof(Vector3) * p_a.leaf_sh_high_order.size()) == 0 &&
			memcmp(p_a.leaf_source_index.ptr(), p_b.leaf_source_index.ptr(), sizeof(uint32_t) * p_a.leaf_source_index.size()) == 0;
}

inline String temp_path(const String &p_name) {
	return TestGaussianSplatHlod::hlod_temp_path(p_name);
}

} // namespace TestHlodMatchedArms

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODMatchedArms] capture seam off and on bake byte-identical trees") {
	using namespace TestHlodMatchedArms;
	for (bool alpha_control : { false, true }) {
		Fixture f;
		make_fixture(f, 3u, 21u, alpha_control);
		BakeResult off_a, off_b, on;
		CandidateCapture capture;
		capture.max_bytes = uint64_t(1) << 26u;
		String reason;
		if (!bake_world(f.input, quality_params(nullptr), off_a, &reason) ||
				!bake_world(f.input, quality_params(nullptr), off_b, &reason) ||
				!bake_world(f.input, quality_params(&capture), on, &reason)) {
			FAIL(reason.utf8().get_data());
			return;
		}
		CHECK(same_bytes(off_a, off_b)); // determinism control: the comparison can say "identical"
		CHECK(same_bytes(off_a, on));
		// Discrimination: the comparison is not vacuous. Perturb one interior byte.
		BakeResult perturbed;
		if (!bake_world(f.input, quality_params(nullptr), perturbed, &reason)) {
			FAIL(reason.utf8().get_data());
			return;
		}
		if (perturbed.tree.interior_gaussians.is_empty()) {
			FAIL("fixture must have interior payload");
			return;
		}
		perturbed.tree.interior_gaussians[0].opacity += 0.125f;
		CHECK_FALSE(same_bytes(off_a, perturbed));
		// The capture holds both candidates for every interior node and the winner flag matches the tree.
		uint32_t interiors = 0;
		for (uint32_t i = 0; i < on.tree.nodes.size(); ++i) {
			if (on.tree.nodes[i].is_leaf()) {
				CHECK_FALSE(capture.nodes[i].captured);
				continue;
			}
			++interiors;
			CHECK(capture.nodes[i].captured);
			CHECK(capture.nodes[i].sat.size() == on.tree.nodes[i].payload_count);
			CHECK(capture.nodes[i].area.size() == on.tree.nodes[i].payload_count);
		}
		CHECK(capture.captured_nodes == interiors);
		CHECK(capture.captured_bytes > 0u);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODMatchedArms] capture seam fails closed on bad opt-in and byte cap") {
	using namespace TestHlodMatchedArms;
	Fixture f;
	make_fixture(f, 3u, 21u, false);
	String reason;
	BakeResult r;
	CandidateCapture no_cap; // max_bytes == 0 is not a contract
	CHECK_FALSE(bake_world(f.input, quality_params(&no_cap), r, &reason));
	CHECK(reason.contains("byte cap"));
	CandidateCapture merge_only_capture;
	merge_only_capture.max_bytes = uint64_t(1) << 26u;
	BakeParams merge_only = quality_params(&merge_only_capture);
	merge_only.mode = BakeParams::MERGE_ONLY;
	CHECK_FALSE(bake_world(f.input, merge_only, r, &reason));
	CandidateCapture tiny;
	tiny.max_bytes = 1024u; // less than one interior node's two candidates
	Error code = OK;
	CHECK_FALSE(bake_world(f.input, quality_params(&tiny), r, &reason, &code));
	CHECK(code == ERR_OUT_OF_MEMORY);
	CHECK(reason.contains("exceeds its"));
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODMatchedArms] arms are matched over one common cut, all SH degrees") {
	using namespace TestHlodMatchedArms;
	struct Layout {
		uint32_t first, high;
	};
	const Layout layouts[] = { { 0u, 0u }, { 3u, 0u }, { 3u, 5u }, { 3u, 12u }, { 3u, 21u } };
	uint32_t merged_winners = 0, selected_winners = 0, differing_arms = 0, cuts_checked = 0;
	for (const Layout &layout : layouts) {
		for (bool alpha_control : { false, true }) {
			CAPTURE(layout.high);
			CAPTURE(alpha_control);
			Fixture f;
			make_fixture(f, layout.first, layout.high, alpha_control);
			CandidateCapture capture;
			capture.max_bytes = uint64_t(1) << 26u;
			BakeResult bake;
			String reason;
			if (!bake_world(f.input, quality_params(&capture), bake, &reason)) {
				FAIL(reason.utf8().get_data());
				return;
			}
			const GaussianSplatHlodTree &tree = bake.tree;
			const uint32_t root_height = tree.nodes[0].height;
			REQUIRE(root_height >= 2u);
			LocalVector<LocalVector<uint32_t>> cuts;
			for (uint32_t h : { 0u, 1u, 2u, root_height }) {
				LocalVector<uint32_t> cut;
				REQUIRE(cut_by_height(tree, h, cut, reason) == Status::OK);
				cuts.push_back(cut);
			}
			View near_view; // heuristic cut, mixed depth
			near_view.position = Vector3(0, 0, 40);
			near_view.forward = Vector3(0, 0, -1);
			near_view.focal_pixels = 800;
			near_view.tau_pixels = 4;
			LocalVector<uint32_t> screen_cut;
			REQUIRE(select_cut(tree, near_view, screen_cut, reason) == Status::OK);
			cuts.push_back(screen_cut);
			for (const LocalVector<uint32_t> &cut : cuts) {
				ArmSet arms;
				const Status status = assemble(f.input, bake, capture, cut, Admission::default_contract(), arms, reason);
				if (status != Status::OK) {
					FAIL(reason.utf8().get_data());
					return;
				}
				++cuts_checked;
				const uint64_t count = arms.domain.cut_splats;
				// Equal candidate count in every arm, the cut domain is the whole input, the reference is all-leaf.
				CHECK(arms.sat.gaussians.size() == count);
				CHECK(arms.area.gaussians.size() == count);
				CHECK(arms.s1b.gaussians.size() == count);
				CHECK(arms.reference.gaussians.size() == f.g.size());
				CHECK(arms.domain.domain_splats == f.g.size());
				uint64_t leaf_splats = 0, leaf_nodes = 0, candidates = 0;
				for (const CutNodeRecord &record : arms.domain.nodes) {
					leaf_splats += record.leaf_splat_count;
					leaf_nodes += record.leaf_node_count;
					candidates += record.candidate_count;
					CHECK(record.candidate_count == tree.nodes[record.node].payload_count);
				}
				CHECK(leaf_splats == f.g.size());
				CHECK(candidates == count);
				CHECK(leaf_nodes == arms.domain.leaf_node_ids.size());
				uint64_t distinct_leaf_splats = 0;
				for (uint32_t leaf : arms.domain.leaf_node_ids) {
					CHECK(tree.nodes[leaf].is_leaf());
					distinct_leaf_splats += tree.nodes[leaf].payload_count;
				}
				CHECK(distinct_leaf_splats == f.g.size()); // exact descendant leaves tile the domain
				// Independent expectation of S1b straight from the baked tree (not from the capture).
				uint64_t out = 0;
				const uint32_t high = layout.high;
				for (const CutNodeRecord &record : arms.domain.nodes) {
					const GaussianSplatHlodNode &node = tree.nodes[record.node];
					double center[3] = {};
					const Gaussian *src;
					const Vector3 *src_sh = nullptr;
					if (node.is_leaf()) {
						src = bake.leaf_gaussians.ptr() + node.payload_first;
						src_sh = high ? bake.leaf_sh_high_order.ptr() + node.payload_first * high : nullptr;
					} else {
						src = tree.interior_gaussians.ptr() + (node.payload_first - tree.leaf_splat_count);
						src_sh = high ? tree.interior_sh_high_order.ptr() + (node.payload_first - tree.leaf_splat_count) * high : nullptr;
						tree.node_cell_center(node, center);
						const bool tree_says_merged = (node.flags & kNodeFlagKindMask) == NODE_KIND_MERGED;
						CHECK(record.use_merged == tree_says_merged);
						merged_winners += tree_says_merged ? 1u : 0u;
						selected_winners += tree_says_merged ? 0u : 1u;
					}
					for (uint32_t k = 0; k < node.payload_count; ++k) {
						Gaussian expect = src[k];
						if (!node.is_leaf()) {
							for (int a = 0; a < 3; ++a) {
								expect.position[a] = real_t(center[a] + double(src[k].position[a]));
							}
						}
						CHECK(memcmp(&expect, &arms.s1b.gaussians[out + k], sizeof(Gaussian)) == 0);
						if (high) {
							CHECK(memcmp(src_sh + uint64_t(k) * high, arms.s1b.sh_high_order.ptr() + (out + k) * high,
										  sizeof(Vector3) * high) == 0);
						}
						if (node.is_leaf()) { // leaves are the original splats in every arm
							CHECK(memcmp(&arms.sat.gaussians[out + k], &expect, sizeof(Gaussian)) == 0);
							CHECK(memcmp(&arms.area.gaussians[out + k], &expect, sizeof(Gaussian)) == 0);
						}
					}
					// The winning arm equals S1b on this node; the loser is a genuinely different candidate.
					const ArmPayload &winner = record.is_leaf || record.use_merged ? arms.sat : arms.area;
					CHECK(memcmp(winner.gaussians.ptr() + out, arms.s1b.gaussians.ptr() + out, sizeof(Gaussian) * node.payload_count) == 0);
					if (!record.is_leaf &&
							memcmp(arms.sat.gaussians.ptr() + out, arms.area.gaussians.ptr() + out, sizeof(Gaussian) * node.payload_count) != 0) {
						++differing_arms;
					}
					out += node.payload_count;
				}
				// Reference equals the original input in leaf order, independent of the capture.
				for (uint32_t i = 0; i < f.g.size(); ++i) {
					CHECK(memcmp(&arms.reference.gaussians[i], &f.g[bake.leaf_source_index[i]], sizeof(Gaussian)) == 0);
				}
			}
		}
	}
	CHECK(cuts_checked > 0u);
	CHECK(merged_winners > 0u); // both winner kinds, and SAT/A-area differ somewhere: not a tautological test
	CHECK(selected_winners > 0u);
	CHECK(differing_arms > 0u);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODMatchedArms] a mismatch fails explicitly, nothing is pruned or filled") {
	using namespace TestHlodMatchedArms;
	Fixture f;
	make_fixture(f, 3u, 21u, true); // alpha control: SAT and A-area differ
	CandidateCapture capture;
	capture.max_bytes = uint64_t(1) << 26u;
	BakeResult bake;
	String reason;
	if (!bake_world(f.input, quality_params(&capture), bake, &reason)) {
		FAIL(reason.utf8().get_data());
		return;
	}
	const GaussianSplatHlodTree &tree = bake.tree;
	LocalVector<uint32_t> cut;
	REQUIRE(cut_by_height(tree, 1u, cut, reason) == Status::OK);
	ArmSet arms;
	REQUIRE(assemble(f.input, bake, capture, cut, Admission::default_contract(), arms, reason) == Status::OK);
	uint32_t interior = UINT32_MAX;
	for (uint32_t id : cut) {
		if (!tree.nodes[id].is_leaf()) {
			interior = id;
		}
	}
	REQUIRE(interior != UINT32_MAX);

	{ // Count mismatch between the candidates.
		CandidateCapture bad = capture;
		bad.nodes[interior].area.gaussians.resize(bad.nodes[interior].area.gaussians.size() - 1);
		CHECK(assemble(f.input, bake, bad, cut, Admission::default_contract(), arms, reason) == Status::COUNT_MISMATCH);
		CHECK(arms.sat.gaussians.is_empty()); // no partial arm survives a failure
	}
	{ // Winner flag contradicts the baked kind.
		CandidateCapture bad = capture;
		bad.nodes[interior].use_merged = !bad.nodes[interior].use_merged;
		CHECK(assemble(f.input, bake, bad, cut, Admission::default_contract(), arms, reason) == Status::CAPTURE_MISMATCH);
	}
	{ // Winner payload differs from what the baker stored.
		CandidateCapture bad = capture;
		LocalVector<Gaussian> &payload = bad.nodes[interior].sat.gaussians;
		CandidateCapture::Node &node = bad.nodes[interior];
		if (node.use_merged) {
			payload[0].opacity += 0.0625f;
		} else {
			node.area.gaussians.write[0].opacity += 0.0625f;
		}
		CHECK(assemble(f.input, bake, bad, cut, Admission::default_contract(), arms, reason) == Status::CAPTURE_MISMATCH);
	}
	{ // A node without captured candidates.
		CandidateCapture bad = capture;
		bad.nodes[interior].captured = false;
		CHECK(assemble(f.input, bake, bad, cut, Admission::default_contract(), arms, reason) == Status::CAPTURE_MISMATCH);
	}
	{ // Incomplete cut: dropping a node must fail, never fill.
		LocalVector<uint32_t> short_cut = cut;
		short_cut.resize(short_cut.size() - 1);
		CHECK(assemble(f.input, bake, capture, short_cut, Admission::default_contract(), arms, reason) == Status::INVALID_CUT);
		LocalVector<uint32_t> missing_first;
		for (uint32_t k = 1; k < cut.size(); ++k) {
			missing_first.push_back(cut[k]);
		}
		CHECK(assemble(f.input, bake, capture, missing_first, Admission::default_contract(), arms, reason) == Status::INVALID_CUT);
	}
	{ // Overlapping cut (ancestor and descendant) and duplicate / out-of-range ids.
		LocalVector<uint32_t> overlap = cut;
		overlap.push_back(0u);
		CHECK(assemble(f.input, bake, capture, overlap, Admission::default_contract(), arms, reason) == Status::INVALID_CUT);
		LocalVector<uint32_t> dup = cut;
		dup.push_back(cut[0]);
		CHECK(assemble(f.input, bake, capture, dup, Admission::default_contract(), arms, reason) == Status::INVALID_CUT);
		LocalVector<uint32_t> oob = cut;
		oob.push_back(tree.nodes.size());
		CHECK(assemble(f.input, bake, capture, oob, Admission::default_contract(), arms, reason) == Status::INVALID_CUT);
	}
	{ // Reference that is not the bake input.
		Fixture other;
		make_fixture(other, 3u, 21u, true);
		other.g[3].opacity = 0.75f;
		CHECK(assemble(other.input, bake, capture, cut, Admission::default_contract(), arms, reason) == Status::REFERENCE_MISMATCH);
	}
	{ // Capture that belongs to another bake.
		CandidateCapture other;
		other.max_bytes = uint64_t(1) << 26u;
		other.nodes.resize(3);
		CHECK(assemble(f.input, bake, other, cut, Admission::default_contract(), arms, reason) == Status::INVALID_INPUT);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODMatchedArms] bounded admission is separate, explicit and refuses over the cap") {
	using namespace TestHlodMatchedArms;
	// The runtime-cut oracle's limits are unchanged defaults.
	CHECK(runtime_cut::MAX_INPUT_SPLATS == 65536u);
	CHECK(runtime_cut::MAX_NODES == 4096u);
	CHECK(runtime_cut::MAX_ALLOCATION_BYTES == 128u * 1024u * 1024u);
	const Admission def = Admission::default_contract();
	CHECK(def.max_total_splats == runtime_cut::MAX_INPUT_SPLATS);
	CHECK(def.max_nodes == runtime_cut::MAX_NODES);
	CHECK(def.max_bytes == runtime_cut::MAX_ALLOCATION_BYTES);
	const Admission large = Admission::large_contract();
	CHECK(large.max_total_splats == 8000000u);
	CHECK(large.max_nodes == 262144u);
	CHECK(large.max_bytes == (uint64_t(12) << 30u));
	String reason;
	// Default contract: refuses 65,537 and accepts 65,536 total.
	CHECK(admit(def, 60000u, 5537u, 100u, 12u, 0u, 0u, reason) == Status::ADMISSION_REFUSED);
	CHECK(admit(def, 1000u, 1000u, 100u, 12u, 100u, 0u, reason) == Status::OK);
	// The 8M contract accepts exactly 8,000,000 leaves + interiors and refuses one more.
	CHECK(admit(large, 6000000u, 2000000u, 1000u, 12u, 0u, 0u, reason) == Status::OK);
	CHECK(admit(large, 6000000u, 2000001u, 1000u, 12u, 0u, 0u, reason) == Status::ADMISSION_REFUSED);
	CHECK(admit(large, 1000u, 100u, 262145u, 12u, 0u, 0u, reason) == Status::ADMISSION_REFUSED);
	// Bytes: 8M leaves + a 8M-splat cut at SH3 (288 B/splat) is far over 12 GiB.
	CHECK(admit(large, 8000000u, 0u, 1000u, 12u, 8000000u, 0u, reason) == Status::OK); // 9.2 GB of arms fit alone
	CHECK(admit(large, 8000000u, 0u, 1000u, 12u, 8000000u, uint64_t(4) << 30u, reason) == Status::ADMISSION_REFUSED);
	CHECK(admit(large, 2000000u, 600000u, 1000u, 12u, 300000u, 1u << 28u, reason) == Status::OK);
	// An unset contract is refused; there is no implicit "unlimited".
	CHECK(admit(Admission(), 1u, 0u, 1u, 0u, 0u, 0u, reason) == Status::ADMISSION_REFUSED);

	// Through assemble(): a one-splat-too-small contract refuses before any arm is allocated.
	Fixture f;
	make_fixture(f, 3u, 21u, false);
	CandidateCapture capture;
	capture.max_bytes = uint64_t(1) << 26u;
	BakeResult bake;
	if (!bake_world(f.input, quality_params(&capture), bake, &reason)) {
		FAIL(reason.utf8().get_data());
		return;
	}
	LocalVector<uint32_t> cut;
	REQUIRE(cut_by_height(bake.tree, 1u, cut, reason) == Status::OK);
	Admission tight;
	tight.max_nodes = 4096u;
	tight.max_bytes = uint64_t(1) << 30u;
	tight.max_total_splats = uint64_t(bake.tree.leaf_splat_count) + bake.tree.interior_splat_count - 1u;
	ArmSet arms;
	CHECK(assemble(f.input, bake, capture, cut, tight, arms, reason) == Status::ADMISSION_REFUSED);
	CHECK(arms.sat.gaussians.is_empty());
	tight.max_total_splats += 1u;
	CHECK(assemble(f.input, bake, capture, cut, tight, arms, reason) == Status::OK);
	Admission byte_tight = tight;
	byte_tight.max_bytes = 4096u; // far below the arm bytes
	CHECK(assemble(f.input, bake, capture, cut, byte_tight, arms, reason) == Status::ADMISSION_REFUSED);
	Admission node_tight = tight;
	node_tight.max_nodes = bake.tree.nodes.size() - 1u;
	CHECK(assemble(f.input, bake, capture, cut, node_tight, arms, reason) == Status::ADMISSION_REFUSED);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODMatchedArms] every arm round-trips natively, strictly") {
	using namespace TestHlodMatchedArms;
	struct Layout {
		uint32_t first, high;
	};
	const Layout layouts[] = { { 0u, 0u }, { 3u, 0u }, { 3u, 5u }, { 3u, 12u }, { 3u, 21u } };
	for (const Layout &layout : layouts) {
		CAPTURE(layout.high);
		Fixture f;
		make_fixture(f, layout.first, layout.high, false);
		CandidateCapture capture;
		capture.max_bytes = uint64_t(1) << 26u;
		BakeResult bake;
		String reason;
		if (!bake_world(f.input, quality_params(&capture), bake, &reason)) {
			FAIL(reason.utf8().get_data());
			return;
		}
		LocalVector<uint32_t> cut;
		REQUIRE(cut_by_height(bake.tree, 1u, cut, reason) == Status::OK);
		ArmSet arms;
		REQUIRE(assemble(f.input, bake, capture, cut, Admission::default_contract(), arms, reason) == Status::OK);
		const ArmPayload *payloads[4] = { &arms.sat, &arms.area, &arms.s1b, &arms.reference };
		for (int a = 0; a < 4; ++a) {
			const String path = temp_path(vformat("arm%d_%d", a, layout.high));
			if (write_arm(*payloads[a], f.meta, path, reason) != Status::OK) {
				FAIL(reason.utf8().get_data());
				return;
			}
			const Status status = verify_roundtrip(*payloads[a], f.meta, path, reason);
			if (status != Status::OK) {
				print_line(vformat("roundtrip arm=%d high=%d: %s: %s", a, layout.high, status_name(status), reason));
				FAIL(reason.utf8().get_data());
				return;
			}
			if (a == 0) { // discrimination of the verifier, once per layout
				ArmMeta aa_flipped = f.meta;
				aa_flipped.antialiased = !aa_flipped.antialiased;
				CHECK(verify_roundtrip(*payloads[a], aa_flipped, path, reason) == Status::ROUNDTRIP_MISMATCH);
				ArmMeta metadata_changed = f.meta;
				metadata_changed.metadata["source"] = "other";
				CHECK(verify_roundtrip(*payloads[a], metadata_changed, path, reason) == Status::ROUNDTRIP_MISMATCH);
				{
					ArmMeta aa_source = f.meta;
					aa_source.antialiased = true; // must be refused, never silently dropped
					CHECK(write_arm(*payloads[a], aa_source, path + ".aa", reason) == Status::INVALID_INPUT);
					CHECK_FALSE(FileAccess::exists(path + ".aa"));
				}
				ArmMeta mode_2d = f.meta;
				mode_2d.is_2d = true;
				CHECK(verify_roundtrip(*payloads[a], mode_2d, path, reason) == Status::ROUNDTRIP_MISMATCH);
				ArmPayload tag_flipped;
				tag_flipped.gaussians = payloads[a]->gaussians;
				tag_flipped.sh_high_order = payloads[a]->sh_high_order;
				tag_flipped.gaussians[0].render_meta ^= GAUSSIAN_RENDER_META_DC_ENCODING_MASK;
				CHECK(verify_roundtrip(tag_flipped, f.meta, path, reason) == Status::ROUNDTRIP_MISMATCH);
				if (layout.high) {
					ArmPayload sh_changed;
					sh_changed.gaussians = payloads[a]->gaussians;
					sh_changed.sh_high_order = payloads[a]->sh_high_order;
					sh_changed.sh_high_order[0].x += 0.5f;
					CHECK(verify_roundtrip(sh_changed, f.meta, path, reason) == Status::ROUNDTRIP_MISMATCH);
				}
			}
			DirAccess::remove_absolute(path);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Headless driver. NOT_RUN unless GS_HLOD_QEXPORT=1. See the file header for the command.
// ------------------------------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][WorldIO][HLOD][HLODQualityExport] env-driven matched-arm export (NOT_RUN unless GS_HLOD_QEXPORT=1)") {
	using namespace TestHlodMatchedArms;
	OS *os = OS::get_singleton();
	if (os->get_environment("GS_HLOD_QEXPORT") != "1") {
		print_line("HLOD_QEXPORT NOT_RUN: set GS_HLOD_QEXPORT=1 with _WORLD, _VIEWS and a fresh _OUTPUT.");
		return;
	}
	const String world_path = os->get_environment("GS_HLOD_QEXPORT_WORLD");
	const String views_path = os->get_environment("GS_HLOD_QEXPORT_VIEWS");
	const String output_dir = os->get_environment("GS_HLOD_QEXPORT_OUTPUT");
	const String asset_filter = os->get_environment("GS_HLOD_QEXPORT_ASSET");
	const String contract_name = os->get_environment("GS_HLOD_QEXPORT_CONTRACT");
	if (world_path.is_empty() || views_path.is_empty() || output_dir.is_empty() ||
			!(contract_name.is_empty() || contract_name == "default" || contract_name == "large")) {
		FAIL("GS_HLOD_QEXPORT needs GS_HLOD_QEXPORT_WORLD, _VIEWS, _OUTPUT and CONTRACT in {default, large}.");
		return;
	}
	if (DirAccess::dir_exists_absolute(output_dir) || FileAccess::exists(output_dir)) {
		FAIL("GS_HLOD_QEXPORT_OUTPUT must not exist: exports go to a fresh directory.");
		return;
	}
	const Admission contract = contract_name == "large" ? Admission::large_contract() : Admission::default_contract();

	// Views: rows[] of {asset, distance_label, tau_pixels, position[3], target[3] | forward[3], [focal_pixels]}.
	Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(views_path));
	if (parsed.get_type() != Variant::DICTIONARY) {
		FAIL("Views file is not a JSON object.");
		return;
	}
	const Dictionary views_doc = parsed;
	const Dictionary camera = views_doc.get("camera_rule", Dictionary());
	const Array viewport = camera.get("viewport", Array());
	const double fov = double(camera.get("vertical_fov_degrees", 60.0));
	const double height = viewport.size() == 2 ? double(viewport[1]) : 540.0;
	const double default_focal = 0.5 * height / Math::tan(Math::deg_to_rad(0.5 * fov));
	const double near_plane = double(camera.get("near", 0.01));
	struct Row {
		String id;
		View view;
	};
	LocalVector<Row> rows;
	const Array row_list = views_doc.get("rows", Array());
	for (int i = 0; i < row_list.size(); ++i) {
		const Dictionary row = row_list[i];
		if (!asset_filter.is_empty() && String(row.get("asset", "")) != asset_filter) {
			continue;
		}
		const Array pos = row.get("position", Array());
		Array dir = row.get("forward", Array());
		if (dir.is_empty()) {
			const Array target = row.get("target", Array());
			if (target.size() == 3 && pos.size() == 3) {
				for (int a = 0; a < 3; ++a) {
					dir.push_back(double(target[a]) - double(pos[a]));
				}
			}
		}
		if (pos.size() != 3 || dir.size() != 3) {
			FAIL("A view row lacks position and target/forward.");
			return;
		}
		Row r;
		r.view.position = Vector3(real_t(double(pos[0])), real_t(double(pos[1])), real_t(double(pos[2])));
		r.view.forward = Vector3(real_t(double(dir[0])), real_t(double(dir[1])), real_t(double(dir[2])));
		r.view.focal_pixels = double(row.get("focal_pixels", default_focal));
		r.view.tau_pixels = double(row.get("tau_pixels", 1.0));
		r.view.near_plane = near_plane;
		r.id = String(row.get("id", vformat("%s_%s_tau%s", String(row.get("asset", "view")), String(row.get("distance_label", itos(i))),
									   String(row.get("tau_pixels", 1)))));
		rows.push_back(r);
	}
	if (rows.is_empty()) {
		FAIL("No view rows selected.");
		return;
	}

	// Source world: resident payload, compact SH0-4 only.
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error load_err = ERR_BUG;
	Ref<GaussianSplatWorld> source = loader.load(world_path, "", &load_err);
	if (load_err != OK || source.is_null() || (source->get_gaussian_data().is_null() && source->materialize_resident_gaussian_data() != OK)) {
		FAIL("Cannot load a resident source world.");
		return;
	}
	GaussianData::SaveSnapshot snapshot;
	if (source->get_gaussian_data()->capture_save_snapshot(snapshot) != OK) {
		FAIL("Cannot snapshot the source payload.");
		return;
	}
	ArmMeta meta;
	meta.sh_first_order_count = snapshot.get_sh_first_order_count();
	meta.sh_high_order_count = snapshot.get_sh_high_order_count();
	meta.is_2d = snapshot.get_2d_mode();
	meta.antialiased = snapshot.get_antialiased();
	meta.metadata = source->get_metadata();
	const uint64_t splats = snapshot.get_gaussians().size();
	String reason;
	// Admission before the bake: the leaves alone must fit (interiors are re-checked after the bake).
	if (admit(contract, splats, 0u, 1u, meta.sh_high_order_count, 0u, 0u, reason) != Status::OK) {
		FAIL(vformat("NOT_ADMITTED: %s", reason).utf8().get_data());
		return;
	}
	BakeInput input;
	input.gaussians = snapshot.get_gaussians().ptr();
	input.count = uint32_t(splats);
	input.sh_high_order = meta.sh_high_order_count ? snapshot.get_sh_high_order().ptr() : nullptr;
	input.sh_first_order_count = meta.sh_first_order_count;
	input.sh_high_order_count = meta.sh_high_order_count;
	CandidateCapture capture;
	capture.max_bytes = contract.max_bytes;
	BakeParams params;
	params.mode = BakeParams::QUALITY_SELECTION;
	params.capture = &capture;
	BakeResult bake;
	if (!bake_world(input, params, bake, &reason)) {
		FAIL(vformat("Quality bake failed: %s", reason).utf8().get_data());
		return;
	}
	print_line(vformat("HLOD_QEXPORT bake nodes=%d leaves=%d interiors=%d capture_bytes=%d", bake.tree.nodes.size(),
			bake.tree.leaf_splat_count, bake.tree.interior_splat_count, capture.captured_bytes));

	DirAccess::make_dir_recursive_absolute(output_dir);
	String reference_sha;
	bool reference_written = false;
	for (const Row &row : rows) {
		LocalVector<uint32_t> cut;
		if (select_cut(bake.tree, row.view, cut, reason) != Status::OK) {
			FAIL(vformat("%s: %s", row.id, reason).utf8().get_data());
			return;
		}
		ArmSet arms;
		const Status assembled = assemble(input, bake, capture, cut, contract, arms, reason);
		if (assembled != Status::OK) {
			FAIL(vformat("%s: %s: %s", row.id, status_name(assembled), reason).utf8().get_data());
			return;
		}
		const String row_dir = output_dir.path_join(row.id);
		DirAccess::make_dir_recursive_absolute(row_dir);
		Dictionary manifest;
		manifest["id"] = row.id;
		manifest["contract"] = contract_name.is_empty() ? "default" : contract_name;
		manifest["source_world"] = world_path;
		manifest["source_sha256"] = FileAccess::get_sha256(world_path);
		manifest["tau_pixels"] = row.view.tau_pixels;
		manifest["focal_pixels"] = row.view.focal_pixels;
		manifest["domain_splats"] = int64_t(arms.domain.domain_splats);
		manifest["cut_splats"] = int64_t(arms.domain.cut_splats);
		manifest["cut_nodes"] = int64_t(arms.domain.nodes.size());
		manifest["cut_kinds_leaf_merged_selected"] = Array({ int64_t(arms.domain.kinds[0]), int64_t(arms.domain.kinds[1]), int64_t(arms.domain.kinds[2]) });
		manifest["sh_first_order_count"] = int(meta.sh_first_order_count);
		manifest["sh_high_order_count"] = int(meta.sh_high_order_count);
		manifest["antialiased"] = meta.antialiased;
		manifest["is_2d"] = meta.is_2d;
		Array nodes;
		for (const CutNodeRecord &record : arms.domain.nodes) {
			Dictionary entry;
			entry["node"] = int64_t(record.node);
			entry["kind"] = record.is_leaf ? "leaf" : (record.use_merged ? "sat" : "a_area");
			entry["candidate_count"] = int64_t(record.candidate_count);
			entry["leaf_splat_first"] = int64_t(record.leaf_splat_first);
			entry["leaf_splat_count"] = int64_t(record.leaf_splat_count);
			entry["leaf_node_first"] = int64_t(record.leaf_node_first);
			entry["leaf_node_count"] = int64_t(record.leaf_node_count);
			nodes.push_back(entry);
		}
		manifest["cut"] = nodes;
		PackedInt32Array leaf_ids;
		for (uint32_t leaf : arms.domain.leaf_node_ids) {
			leaf_ids.push_back(int32_t(leaf));
		}
		manifest["leaf_node_ids"] = leaf_ids;
		struct ArmOut {
			const char *name;
			const ArmPayload *payload;
		};
		const ArmOut outs[3] = { { "sat", &arms.sat }, { "a_area", &arms.area }, { "s1b", &arms.s1b } };
		Dictionary files;
		for (const ArmOut &o : outs) {
			const String path = row_dir.path_join(String(o.name) + ".gsplatworld");
			Status status = write_arm(*o.payload, meta, path, reason);
			if (status == Status::OK) {
				status = verify_roundtrip(*o.payload, meta, path, reason);
			}
			if (status != Status::OK) {
				FAIL(vformat("%s/%s: %s: %s", row.id, o.name, status_name(status), reason).utf8().get_data());
				return;
			}
			Dictionary entry;
			entry["path"] = path;
			entry["sha256"] = FileAccess::get_sha256(path);
			entry["splats"] = int64_t(o.payload->gaussians.size());
			files[o.name] = entry;
		}
		if (!reference_written) { // the all-leaf reference spans the whole domain: identical for every row
			const String path = output_dir.path_join("reference.gsplatworld");
			Status status = write_arm(arms.reference, meta, path, reason);
			if (status == Status::OK) {
				status = verify_roundtrip(arms.reference, meta, path, reason);
			}
			if (status != Status::OK) {
				FAIL(vformat("reference: %s: %s", status_name(status), reason).utf8().get_data());
				return;
			}
			reference_sha = FileAccess::get_sha256(path);
			reference_written = true;
		}
		Dictionary reference_entry;
		reference_entry["path"] = output_dir.path_join("reference.gsplatworld");
		reference_entry["sha256"] = reference_sha;
		reference_entry["splats"] = int64_t(arms.reference.gaussians.size());
		files["reference"] = reference_entry;
		manifest["files"] = files;
		Ref<FileAccess> out = FileAccess::open(row_dir.path_join("manifest.json"), FileAccess::WRITE);
		if (out.is_null()) {
			FAIL("Cannot write the row manifest.");
			return;
		}
		out->store_string(JSON::stringify(manifest, "\t"));
		print_line(vformat("HLOD_QEXPORT_ROW id=%s cut_nodes=%d cut_splats=%d domain=%d reference=%d", row.id, arms.domain.nodes.size(),
				arms.domain.cut_splats, arms.domain.domain_splats, arms.reference.gaussians.size()));
	}
	print_line(vformat("HLOD_QEXPORT_DONE rows=%d output=%s", rows.size(), output_dir));
}
