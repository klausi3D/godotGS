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

TEST_CASE("[Streaming Pipeline] Chunk draw admission ignores invisible demand and charges shared instances (#1486)") {
	const LODConfig config = make_config();
	LocalVector<ChunkLODCandidate> candidates = make_candidates();
	candidates.resize(3);
	candidates[0].visible = false;
	candidates[1].distance = 2.0f;
	candidates[2] = candidates[1];
	candidates[2].distance = 1.0f; // Higher priority despite its higher index.
	ChunkLODPlan plan;
	select_chunk_lods(candidates, config, 4, 300000, plan);
	if (plan.selections.size() != 3) {
		FAIL("Admission must retain entries for invisible and visible candidates");
		return;
	}
	CHECK(plan.selections[0].admission == ChunkLODAdmission::NOT_VISIBLE);
	CHECK(plan.selections[0].effective_count == 200000);
	CHECK(plan.selections[1].effective_count == 100000);
	CHECK(plan.selections[2].effective_count == 200000);
	CHECK(plan.admitted_splats == 300000);
	CHECK(plan.coarsened_visible_chunks == 1);
	CHECK(plan.refused_visible_chunks == 0);
	// The same atlas chunk can emit once per instance. With two references per
	// splat, even both coarser representations cost 400k, requiring one refusal.
	for (ChunkLODCandidate &candidate : candidates) {
		candidate.draw_instance_count = 2;
	}
	select_chunk_lods(candidates, config, 4, 300000, plan);
	CHECK(plan.selections[0].admission == ChunkLODAdmission::NOT_VISIBLE);
	CHECK(plan.selections[1].admission == ChunkLODAdmission::REFUSED_DRAW_CAP);
	CHECK(plan.selections[1].effective_count == 0);
	CHECK(plan.selections[2].admission == ChunkLODAdmission::ADMITTED);
	CHECK(plan.selections[2].effective_count == 100000);
	CHECK(plan.admitted_splats == 200000);
	CHECK(plan.coarsened_visible_chunks == 1);
	CHECK(plan.refused_visible_chunks == 1);
	// Feeding back the selected LODs must not undo cap coarsening next frame.
	candidates[1].previous_lod = plan.selections[1].lod;
	candidates[2].previous_lod = plan.selections[2].lod;
	select_chunk_lods(candidates, config, 4, 300000, plan);
	CHECK(plan.admitted_splats == 200000);
	CHECK(plan.selections[1].admission == ChunkLODAdmission::REFUSED_DRAW_CAP);
	CHECK(plan.selections[2].effective_count == 100000);
}

TEST_CASE("[Streaming Pipeline] Engaged and fitting chunk admission preserves supplied nominal selections (#1486)") {
	const LODConfig config = make_config();
	LocalVector<ChunkLODCandidate> candidates;
	candidates.resize(3);
	ChunkLODPlan expected;
	expected.selections.resize(3);
	for (uint32_t i = 0; i < candidates.size(); i++) {
		ChunkLODCandidate &candidate = candidates[i];
		candidate.splat_count = 200000;
		candidate.distance = 1.0f; // Would independently select LOD0 and opacity 1.
		candidate.visible = i != 1;
		candidate.has_nominal_selection = true;
		ChunkLODSelection &nominal = candidate.nominal_selection;
		nominal.lod = i == 0 ? 1 : 0;
		nominal.sh_band_level = 3 - int(i);
		nominal.splat_skip_factor = i == 0 ? 2 : 1;
		nominal.opacity_multiplier = 0.35f + float(i) * 0.1f;
		// Preserve the supplied count, even when it differs from count / skip.
		nominal.effective_count = i == 0 ? 75000 : (i == 1 ? 200000 : 110000);
		nominal.admission = candidate.visible ? ChunkLODAdmission::ADMITTED : ChunkLODAdmission::NOT_VISIBLE;
		candidate.previous_lod = nominal.lod;
		candidate.previous_target_lod = nominal.lod;
		expected.selections[i] = nominal;
	}
	constexpr uint64_t cap = 250000;
	// The no-frustum bound engages admission (385k), but visible nominal demand
	// fits (185k). Every supplied parameter must survive unchanged.
	CHECK(uint64_t(candidates[0].nominal_selection.effective_count) + candidates[1].nominal_selection.effective_count +
				candidates[2].nominal_selection.effective_count > cap);
	expected.admitted_splats = 185000;
	ChunkLODPlan plan;
	select_chunk_lods(candidates, config, 4, cap, plan);
	check_same_plan(expected, plan);
	CHECK(plan.coarsened_visible_chunks == 0);
	CHECK(plan.refused_visible_chunks == 0);
}

TEST_CASE("[Streaming Pipeline] Chunk LOD plan refuses only after every visible chunk is at its coarsest LOD, farthest first (#1486)") {
	LODConfig config = make_config();
	config.num_levels = 3;
	if (!config.validate() || !config.splat_skip_enabled || config.get_splat_skip_factor(0) != 1 ||
			config.get_splat_skip_factor(1) != 2 || config.get_splat_skip_factor(2) != 4) {
		FAIL("Fixture needs three LODs reducing each chunk from 200k to 100k to 50k");
		return;
	}
	LocalVector<ChunkLODCandidate> candidates;
	candidates.resize(3);
	constexpr float distances[] = { 2.0f, 3.0f, 1.0f }; // Middle, farthest, nearest: neither index order.
	for (uint32_t i = 0; i < candidates.size(); i++) {
		candidates[i].splat_count = 200000;
		candidates[i].distance = distances[i];
		candidates[i].visible = true;
		if (config.calculate_lod_level(distances[i]) != 0) {
			FAIL("Every visible chunk must start at LOD0 with two coarser alternatives");
			return;
		}
	}
	const uint32_t coarsest_lod = uint32_t(config.num_levels - 1);
	// All three coarsest representations still cost 150k. A 100k cap needs
	// exactly one refusal, after all coarsening, and it must be candidate 1.
	constexpr uint64_t refusal_cap = 100000;
	ChunkLODPlan refused;
	select_chunk_lods(candidates, config, 4, refusal_cap, refused);
	if (refused.selections.size() != candidates.size()) {
		FAIL("Refusal plan must report all three visible chunks");
		return;
	}
	uint64_t admitted_demand = 0;
	uint32_t refused_count = 0;
	for (uint32_t i = 0; i < refused.selections.size(); i++) {
		const ChunkLODSelection &selection = refused.selections[i];
		CHECK(selection.lod == coarsest_lod); // Even the refused chunk was coarsened first.
		if (i == 1) {
			CHECK(selection.admission == ChunkLODAdmission::REFUSED_DRAW_CAP);
			CHECK(selection.effective_count == 0);
		} else {
			CHECK(selection.admission == ChunkLODAdmission::ADMITTED);
			CHECK(selection.effective_count == 50000);
		}
		if (selection.admission == ChunkLODAdmission::ADMITTED) {
			admitted_demand += selection.effective_count;
		} else if (selection.admission == ChunkLODAdmission::REFUSED_DRAW_CAP) {
			refused_count++;
		}
	}
	CHECK(refused_count == 1);
	CHECK(refused.refused_visible_chunks == refused_count);
	CHECK(refused.admitted_splats == admitted_demand);
	CHECK(refused.admitted_splats == 100000);
	CHECK(refused.admitted_splats <= refusal_cap);

	// Retaining every coarsest representation fits exactly; no hole is allowed.
	constexpr uint64_t coarsening_cap = 150000;
	ChunkLODPlan coarsened;
	select_chunk_lods(candidates, config, 4, coarsening_cap, coarsened);
	if (coarsened.selections.size() != candidates.size()) {
		FAIL("Coarsening-only plan must retain all three visible chunks");
		return;
	}
	CHECK(coarsened.refused_visible_chunks == 0);
	CHECK(coarsened.admitted_splats == coarsening_cap);
	for (const ChunkLODSelection &selection : coarsened.selections) {
		CHECK(selection.admission == ChunkLODAdmission::ADMITTED);
		CHECK(selection.lod == coarsest_lod);
		CHECK(selection.effective_count == 50000);
	}
}

} // namespace TestChunkLODSelection
