#pragma once

#include "test_macros.h"

#include "../core/chunk_lod_selection.h"
#include "../lod/lod_config.h"

namespace TestChunkLODSelection {

using namespace GaussianSplatting;

static LODConfig make_config() {
	LODConfig config;
	config.enabled = true;
	config.num_levels = 2;
	config.max_distance = 100.0f;
	config.splat_skip_enabled = true;
	config.opacity_fade_enabled = false;
	return config;
}

static LocalVector<ChunkLODCandidate> make_candidates() {
	LocalVector<ChunkLODCandidate> candidates;
	candidates.resize(2);
	for (ChunkLODCandidate &candidate : candidates) {
		candidate.splat_count = 200000;
		candidate.distance = 1.0f;
		candidate.visible = true;
	}
	return candidates;
}

static void check_same_plan(const ChunkLODPlan &p_first, const ChunkLODPlan &p_second) {
	if (p_first.selections.size() != p_second.selections.size()) {
		FAIL("Repeated/under-cap selection must preserve the plan cardinality");
		return;
	}
	CHECK(p_first.admitted_splats == p_second.admitted_splats);
	CHECK(p_first.refused_visible_chunks == p_second.refused_visible_chunks);
	CHECK(p_first.lod_transitions == p_second.lod_transitions);
	for (uint32_t i = 0; i < p_first.selections.size(); i++) {
		const ChunkLODSelection &first = p_first.selections[i];
		const ChunkLODSelection &second = p_second.selections[i];
		CHECK(first.lod == second.lod);
		CHECK(first.sh_band_level == second.sh_band_level);
		CHECK(first.splat_skip_factor == second.splat_skip_factor);
		CHECK(first.opacity_multiplier == second.opacity_multiplier);
		CHECK(first.effective_count == second.effective_count);
		CHECK(first.admission == second.admission);
	}
}

TEST_CASE("[Streaming Pipeline] Chunk LOD plan fits the drawn cap while retaining visible coverage (#1486)") {
	const LODConfig config = make_config();
	const LocalVector<ChunkLODCandidate> candidates = make_candidates();
	// The existing production LOD policy supplies the alternatives: LOD0 keeps
	// 200k, and LOD1's skip factor of two emits 100k from each visible chunk.
	if (!config.validate() || config.calculate_lod_level(1.0f) != 0 || config.get_splat_skip_factor(0) != 1 ||
			config.get_splat_skip_factor(1) != 2) {
		FAIL("Fixture needs 200k selected demand and an available 100k coarser representation per chunk");
		return;
	}
	constexpr uint64_t cap = 250000;
	ChunkLODPlan plan;
	select_chunk_lods(candidates, config, 4, cap, plan);
	if (plan.selections.size() != 2) {
		FAIL("The plan must report a selection or explicit refusal for both visible candidates");
		return;
	}
	uint64_t admitted_demand = 0;
	for (uint32_t i = 0; i < plan.selections.size(); i++) {
		const ChunkLODSelection &selection = plan.selections[i];
		CHECK(selection.admission == ChunkLODAdmission::ADMITTED);
		CHECK(selection.effective_count > 0);
		CHECK(selection.lod < uint32_t(config.num_levels));
		CHECK(selection.splat_skip_factor == config.get_splat_skip_factor(int(selection.lod)));
		CHECK(selection.effective_count == 200000u / config.get_splat_skip_factor(int(selection.lod)));
		if (selection.admission == ChunkLODAdmission::ADMITTED) {
			admitted_demand += selection.effective_count;
		}
	}
	CHECK(plan.refused_visible_chunks == 0);
	CHECK(plan.admitted_splats == admitted_demand);
	// Intentionally RED in phase 1: unchanged production selects 200k + 200k.
	CHECK_MESSAGE(admitted_demand <= cap,
			"Visible demand must be admitted before depth emission; current demand is ", admitted_demand, " against cap ", cap);
}

TEST_CASE("[Streaming Pipeline] Chunk LOD plan is identical when demand fits the drawn cap (#1486)") {
	const LODConfig config = make_config();
	const LocalVector<ChunkLODCandidate> candidates = make_candidates();
	ChunkLODPlan unrestricted;
	ChunkLODPlan under_cap;
	select_chunk_lods(candidates, config, 4, UINT64_MAX, unrestricted);
	select_chunk_lods(candidates, config, 4, 400000, under_cap);
	if (under_cap.selections.size() != 2 || unrestricted.selections.size() != 2) {
		FAIL("Under-cap control must preserve both production selections");
		return;
	}
	check_same_plan(unrestricted, under_cap);
	CHECK(under_cap.admitted_splats == 400000);
	CHECK(under_cap.refused_visible_chunks == 0);
	for (const ChunkLODSelection &selection : under_cap.selections) {
		CHECK(selection.admission == ChunkLODAdmission::ADMITTED);
		CHECK(selection.lod == 0);
		CHECK(selection.effective_count == 200000);
		CHECK(selection.splat_skip_factor == 1);
		CHECK(selection.sh_band_level == 4);
		CHECK(selection.opacity_multiplier == 1.0f);
	}
}

TEST_CASE("[Streaming Pipeline] Chunk LOD plan reports deterministic refusal without a coarser representation (#1486)") {
	LODConfig config = make_config();
	// With skipping disabled, every configured LOD still costs 200k: there
	// is no splat-reducing representation to fit both candidates into 250k.
	config.splat_skip_enabled = false;
	if (!config.validate() || config.get_splat_skip_factor(1) != 1) {
		FAIL("No-coarser control needs a valid policy with no lower-demand representation");
		return;
	}
	const LocalVector<ChunkLODCandidate> candidates = make_candidates();
	ChunkLODPlan first;
	ChunkLODPlan repeated;
	select_chunk_lods(candidates, config, 4, 250000, first);
	select_chunk_lods(candidates, config, 4, 250000, repeated);
	if (first.selections.size() != 2 || repeated.selections.size() != 2) {
		FAIL("No-coarser control must report an outcome for both visible candidates");
		return;
	}
	check_same_plan(first, repeated);
	uint32_t admitted = 0;
	uint32_t refused = 0;
	uint64_t admitted_demand = 0;
	for (const ChunkLODSelection &selection : first.selections) {
		if (selection.admission == ChunkLODAdmission::ADMITTED) {
			admitted++;
			admitted_demand += selection.effective_count;
		} else if (selection.admission == ChunkLODAdmission::REFUSED_DRAW_CAP) {
			refused++;
			CHECK(selection.effective_count == 0);
		}
	}
	CHECK(admitted == 1);
	CHECK(refused == 1);
	CHECK(first.refused_visible_chunks == refused);
	CHECK(first.admitted_splats == admitted_demand);
	CHECK(admitted_demand == 200000);
}

} // namespace TestChunkLODSelection
