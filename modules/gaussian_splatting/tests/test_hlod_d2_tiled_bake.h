#pragma once

// D2-2 phase 1: runtime-red tests against TiledAutoTree's ERR_UNAVAILABLE stub.
// Registered exactly like test_hlod_d2_bake.h: modules/SCsub globs tests/*.h.
// [WorldIO] selects the existing host-only lane. No fixture files or new guard.
// Identity includes every node-record field and every Gaussian/SH payload byte;
// timings and allocator state are deliberately excluded. No cross-toolchain claim.

#include "test_macros.h"

#include "../core/gaussian_splat_hlod_tiled_bake.h"
#include "core/os/os.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace TestHlodD2TiledBake {

using namespace gs_hlod;

// REQUIRE alone does not stop execution in Godot's no-exceptions test build.
#define D2_TILED_NEED(condition) \
	do { \
		if (!(condition)) { \
			FAIL("D2 tiled precondition failed: ", #condition); \
			return; \
		} \
	} while (0)

inline uint32_t next_random(uint32_t &r_state) {
	r_state ^= r_state << 13;
	r_state ^= r_state >> 17;
	r_state ^= r_state << 5;
	return r_state;
}

struct Input {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh;
	uint32_t high_count = 0;

	BakeInput view() const {
		return { gaussians.ptr(), sh.ptr(), gaussians.size(), high_count, high_count ? 3u : 0u };
	}
};

inline Input make_input(uint32_t p_count, uint32_t p_clusters, uint32_t p_high = 0, float p_shift = 0.0f) {
	Input input;
	input.high_count = p_high;
	input.gaussians.resize(p_count);
	input.sh.resize(p_count * p_high);
	uint32_t random = 0xd202babeu;
	for (uint32_t i = 0; i < p_count; ++i) {
		const uint32_t cluster = i % p_clusters;
		Gaussian &g = input.gaussians[i];
		// Eight clusters: two e=7 parents, each with four e=6 frontier
		// cells of 20,480 splats. Both parents exceed B_max=65,536, so
		// root AND parent levels must be stitched above the eight tiles.
		const float x = float((cluster & 1u) * 128u + ((cluster >> 1u) & 1u) * 64u);
		const float y = float(((cluster >> 2u) & 1u) * 64u);
		const float dx = 1.0f + float(next_random(random) & 1023u) / 256.0f;
		const float dy = 1.0f + float(next_random(random) & 1023u) / 256.0f;
		const float dz = 1.0f + float(next_random(random) & 1023u) / 256.0f;
		g.position = Vector3(x + dx + p_shift, y + dy + p_shift, dz + p_shift);
		// Exact equal importance forces source-index ties. Distinct colours and
		// SH make input reordering observable in actual payload, not just IDs.
		g.scale = Vector3(0.03125f, 0.015625f, 0.0078125f);
		g.rotation = Quaternion(0, 0, 0, 1);
		g.opacity = 0.5f;
		const float red = float(next_random(random) & 65535u) / 65536.0f;
		const float green = float(next_random(random) & 65535u) / 65536.0f;
		g.sh_dc = Color(red, green, float(i % 251u) / 256.0f, 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		for (uint32_t c = 0; c < (p_high ? 3u : 0u); ++c) {
			g.sh_1[c] = Vector3(float(next_random(random) & 255u) / 4096.0f, -0.03125f, 0.0625f);
		}
		for (uint32_t c = 0; c < p_high; ++c) {
			input.sh[i * p_high + c] = Vector3(float(next_random(random) & 255u) / 4096.0f, float(c) / 256.0f, -0.015625f);
		}
	}
	return input;
}

class Source : public TiledBakeSource {
	const Input &input;
	std::vector<uint32_t> order;
	static constexpr uint32_t CHUNK_SIZE = 4093; // crosses cluster/leaf boundaries

public:
	mutable std::atomic<uint32_t> reads{ 0 };

	Source(const Input &p_input, bool p_shuffle) : input(p_input) {
		const uint32_t count = (input.gaussians.size() + CHUNK_SIZE - 1u) / CHUNK_SIZE;
		for (uint32_t i = 0; i < count; ++i) {
			order.push_back(i);
		}
		if (p_shuffle) {
			uint32_t random = 0x9e3779b9u;
			for (uint32_t i = count; i > 1u; --i) {
				std::swap(order[i - 1u], order[next_random(random) % i]);
			}
		}
	}

	bool is_shuffled() const { return !std::is_sorted(order.begin(), order.end()); }

	TiledBakeSourceInfo get_info() const override {
		return { input.gaussians.size(), uint32_t(order.size()), input.high_count, input.high_count ? 3u : 0u };
	}

	Error get_chunk_info(uint32_t p_chunk, TiledBakeChunkInfo &r_info, String *r_error) const override {
		if (p_chunk >= order.size()) {
			if (r_error) {
				*r_error = "Synthetic chunk index out of range.";
			}
			return ERR_INVALID_PARAMETER;
		}
		r_info.first_splat = order[p_chunk] * CHUNK_SIZE;
		r_info.splat_count = MIN(CHUNK_SIZE, input.gaussians.size() - r_info.first_splat);
		r_info.centre_bounds = AABB(input.gaussians[r_info.first_splat].position, Vector3());
		for (uint32_t i = 1; i < r_info.splat_count; ++i) {
			r_info.centre_bounds.expand_to(input.gaussians[r_info.first_splat + i].position);
		}
		return OK;
	}

	Error read_chunk(uint32_t p_chunk, TiledBakeChunk &r_chunk, String *r_error) const override {
		TiledBakeChunkInfo info;
		const Error code = get_chunk_info(p_chunk, info, r_error);
		if (code != OK) {
			return code;
		}
		r_chunk.gaussians.resize(info.splat_count);
		r_chunk.sh_high_order.resize(info.splat_count * input.high_count);
		for (uint32_t i = 0; i < info.splat_count; ++i) {
			r_chunk.gaussians[i] = input.gaussians[info.first_splat + i];
			for (uint32_t c = 0; c < input.high_count; ++c) {
				r_chunk.sh_high_order[i * input.high_count + c] = input.sh[(info.first_splat + i) * input.high_count + c];
			}
		}
		reads.fetch_add(1);
		return OK;
	}
};

inline TiledBakeParams params(uint32_t p_workers = 1) {
	TiledBakeParams p;
	p.max_tile_splats = 65536;
	p.worker_count = p_workers;
	p.limits.max_ram_bytes = uint64_t(2) << 30;
	p.limits.deadline_usec = OS::get_singleton()->get_ticks_usec() + 5400ull * 1000000ull;
	return p;
}

inline bool complete(const BakeResult &p_result, const Input &p_input) {
	String error;
	const auto &t = p_result.tree;
	if (!gs_hlod_validate_tree(t, &error) || t.nodes.is_empty() || !t.interior_resident ||
			t.leaf_splat_count != p_input.gaussians.size() || t.sh_high_order_count != p_input.high_count ||
			p_result.leaf_gaussians.size() != t.leaf_splat_count || p_result.leaf_source_index.size() != t.leaf_splat_count ||
			p_result.leaf_sh_high_order.size() != uint64_t(t.leaf_splat_count) * t.sh_high_order_count ||
			t.interior_gaussians.size() != t.interior_splat_count ||
			t.interior_sh_high_order.size() != uint64_t(t.interior_splat_count) * t.sh_high_order_count) {
		FAIL("Incomplete/invalid tiled bake: ", error.utf8().get_data());
		return false;
	}
	CHECK(t.bake_rule_version == 1u);
	CHECK(t.top_level_nodes.is_empty());
	CHECK(t.instances.is_empty());
	uint32_t next_child = 1;
	uint64_t interior_cursor = t.leaf_splat_count;
	for (const auto &n : t.nodes) {
		if (!n.is_leaf()) {
			CHECK(n.first_child == next_child);
			CHECK(n.payload_first == interior_cursor);
			next_child += n.child_count;
			interior_cursor += n.payload_count;
		}
	}
	CHECK(next_child == t.nodes.size());
	CHECK(interior_cursor == uint64_t(t.leaf_splat_count) + t.interior_splat_count);
	std::vector<bool> seen(t.leaf_splat_count, false);
	for (uint32_t i = 0; i < t.leaf_splat_count; ++i) {
		const uint32_t src = p_result.leaf_source_index[i];
		if (src >= seen.size() || seen[src]) {
			FAIL("Missing/duplicated global leaf source index");
			return false;
		}
		seen[src] = true;
		CHECK(std::memcmp(&p_result.leaf_gaussians[i], &p_input.gaussians[src], sizeof(Gaussian)) == 0);
		for (uint32_t c = 0; c < t.sh_high_order_count; ++c) {
			CHECK(std::memcmp(&p_result.leaf_sh_high_order[i * t.sh_high_order_count + c],
					&p_input.sh[src * t.sh_high_order_count + c], sizeof(Vector3)) == 0);
		}
	}
	return true;
}

inline bool in_core(const Input &p_input, BakeResult &r_result) {
	String error;
	Error code = OK; // bake_world writes r_code only on failure
	if (!bake_world(p_input.view(), BakeParams(), r_result, &error, &code) || code != OK) {
		FAIL("In-core identity oracle failed: ", int(code), " ", error.utf8().get_data());
		return false;
	}
	return complete(r_result, p_input);
}

template <class T>
inline void append(std::vector<uint8_t> &r_bytes, const T &p_value) {
	const auto *begin = reinterpret_cast<const uint8_t *>(&p_value);
	r_bytes.insert(r_bytes.end(), begin, begin + sizeof(T));
}

// Test-only byte stream, not a file format. Do NOT rebase IDs/offsets or sort
// records: doing so would hide BFS/assembly defects. Node fields are explicit
// to avoid C++ padding; Gaussian is the existing pinned 144-byte payload ABI.
// Source indices are checked separately so R9 cannot pass on an ID-only change.
inline std::vector<uint8_t> content_bytes(const BakeResult &p_result) {
	const auto &t = p_result.tree;
	std::vector<uint8_t> bytes;
	append(bytes, t.origin);
	append(bytes, t.bake_rule_version);
	append(bytes, t.leaf_splat_count);
	append(bytes, t.interior_splat_count);
	append(bytes, t.sh_high_order_count);
	append(bytes, t.nodes.size());
	for (const auto &n : t.nodes) {
		append(bytes, n.cell.e);
		append(bytes, n.cell.ix);
		append(bytes, n.cell.iy);
		append(bytes, n.cell.iz);
		append(bytes, n.aabb_min);
		append(bytes, n.aabb_max);
		append(bytes, n.radius);
		append(bytes, n.geometric_error);
		append(bytes, n.error_chosen);
		append(bytes, n.error_rejected);
		append(bytes, n.parent);
		append(bytes, n.first_child);
		append(bytes, n.child_count);
		append(bytes, n.height);
		append(bytes, n.payload_first);
		append(bytes, n.payload_count);
		append(bytes, n.flags);
		append(bytes, n.overlap_footprint);
	}
	for (const auto &g : p_result.leaf_gaussians) {
		append(bytes, g);
	}
	for (const auto &sh : p_result.leaf_sh_high_order) {
		append(bytes, sh);
	}
	for (const auto &g : t.interior_gaussians) {
		append(bytes, g);
	}
	for (const auto &sh : t.interior_sh_high_order) {
		append(bytes, sh);
	}
	return bytes;
}

inline void same_content(const BakeResult &p_expected, const BakeResult &p_actual) {
	CHECK(content_bytes(p_actual) == content_bytes(p_expected));
	D2_TILED_NEED(p_actual.leaf_source_index.size() == p_expected.leaf_source_index.size());
	for (uint32_t i = 0; i < p_expected.leaf_source_index.size(); ++i) {
		CHECK(p_actual.leaf_source_index[i] == p_expected.leaf_source_index[i]);
	}
}

inline bool tiled(const Source &p_source, const Input &p_input, const TiledBakeParams &p_params,
		BakeResult &r_result, TiledBakeReport &r_report) {
	String error;
	const Error code = TiledAutoTree::build(p_source, p_params, r_result, r_report, &error);
	if (code != OK) {
		FAIL("Tiled build must succeed: ", int(code), " ", error.utf8().get_data());
		return false;
	}
	CHECK(r_report.peak_reserved_bytes > 0u);
	CHECK(r_report.peak_reserved_bytes <= p_params.limits.max_ram_bytes);
	CHECK(r_report.largest_tile_splats <= p_params.max_tile_splats);
	return complete(r_result, p_input);
}

inline void no_output(const BakeResult &p_result) {
	CHECK(p_result.tree.nodes.is_empty());
	CHECK(p_result.tree.top_level_nodes.is_empty());
	CHECK(p_result.tree.instances.is_empty());
	CHECK(p_result.tree.interior_gaussians.is_empty());
	CHECK(p_result.tree.interior_sh_high_order.is_empty());
	CHECK(p_result.tree.leaf_splat_count == 0u);
	CHECK(p_result.tree.interior_splat_count == 0u);
	CHECK_FALSE(p_result.tree.interior_resident);
	CHECK(p_result.tree.interior_file_path.is_empty());
	CHECK(p_result.tree.interior_gaussian_offset == 0u);
	CHECK(p_result.tree.interior_sh_offset == 0u);
	CHECK(p_result.leaf_gaussians.is_empty());
	CHECK(p_result.leaf_sh_high_order.is_empty());
	CHECK(p_result.leaf_source_index.is_empty());
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] tiled bytes equal in-core across leaf and multi-level worlds") {
	// Fails on any changed node field, payload byte, SH coefficient, global index,
	// missing tile, single-level-only stitch, or non-BFS assembled interior offset.
	uint32_t count = 257, clusters = 1, high = 0;
	float shift = 0.0f;
	SUBCASE("single SH0 leaf") {
		count = 257;
	}
	SUBCASE("single SH3 tile") {
		count = 40960;
		high = 12;
	}
	SUBCASE("eight SH3 tiles and two stitch levels") {
		count = 163840;
		clusters = 8;
		high = 12;
	}
	SUBCASE("origin-centred root with SH0 tiles") {
		count = 163840;
		clusters = 8;
		shift = -32.0f;
	}
	const Input input = make_input(count, clusters, high, shift);
	Source source(input, true);
	BakeResult expected, actual;
	D2_TILED_NEED(in_core(input, expected));
	if (clusters == 8 && shift == 0.0f) {
		// Independently confirm the fixture forces two levels ABOVE the frontier,
		// rather than mistaking a deep tile subtree for multi-level stitching.
		std::vector<uint32_t> descendants(expected.tree.nodes.size(), 0u);
		for (uint32_t i = expected.tree.nodes.size(); i > 0; --i) {
			const auto &n = expected.tree.nodes[i - 1u];
			if (n.is_leaf()) {
				descendants[i - 1u] = n.payload_count;
			}
			if (n.parent != kNoParent) {
				descendants[n.parent] += descendants[i - 1u];
			}
		}
		uint32_t frontier_at_depth_two = 0;
		for (uint32_t i = 0; i < expected.tree.nodes.size(); ++i) {
			const auto &n = expected.tree.nodes[i];
			if (n.parent == 0u && descendants[i] > 65536u) {
				for (uint32_t c = 0; c < n.child_count; ++c) {
					const uint32_t leaves = descendants[n.first_child + c];
					if (leaves > kMaxNodeSplats && leaves <= 65536u) {
						++frontier_at_depth_two;
					}
				}
			}
		}
		CHECK(frontier_at_depth_two == 8u);
	}
	TiledBakeReport report;
	D2_TILED_NEED(tiled(source, input, params(), actual, report));
	same_content(expected, actual);
	if (clusters == 8 && shift == 0.0f) {
		CHECK(report.tiles_baked >= 8u);
		CHECK(report.stitch_levels >= 2u);
		CHECK(report.largest_tile_splats > kMaxNodeSplats);
		CHECK(source.reads.load() > source.get_info().chunk_count); // actually re-iterated
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] tiled bytes are identical with 1 2 and 8 workers") {
	// Fails if scheduling changes record/payload order, IDs, merge math or offsets.
	const Input input = make_input(163840, 8, 12);
	BakeResult expected;
	D2_TILED_NEED(in_core(input, expected));
	std::vector<uint8_t> serial_bytes;
	for (uint32_t workers : { 1u, 2u, 8u }) {
		CAPTURE(workers);
		Source source(input, true);
		D2_TILED_NEED(source.is_shuffled());
		BakeResult actual;
		TiledBakeReport report;
		D2_TILED_NEED(tiled(source, input, params(workers), actual, report));
		CHECK(report.tiles_baked >= 8u);
		CHECK(report.stitch_levels >= 2u);
		same_content(expected, actual);
		if (workers == 1u) {
			serial_bytes = content_bytes(actual);
		}
		CHECK(content_bytes(actual) == serial_bytes);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] shuffled tile input without ordering changes payload bytes") {
	// Fails if the R9 hook is unwired, the fixture has no observable ties, or
	// the byte comparator ignores order. Both builds MUST succeed; errors or
	// empty results cannot satisfy the inequality. Only input ordering changes.
	const Input input = make_input(163840, 8, 12);
	Source ordered_source(input, false), shuffled_source(input, true);
	D2_TILED_NEED(shuffled_source.is_shuffled());
	BakeResult expected, ordered, repaired, broken;
	D2_TILED_NEED(in_core(input, expected));
	TiledBakeReport report;
	TiledBakeParams p = params(2);
	D2_TILED_NEED(tiled(ordered_source, input, p, ordered, report));
	D2_TILED_NEED(tiled(shuffled_source, input, p, repaired, report));
	same_content(expected, ordered);
	same_content(expected, repaired);
	TiledBakeTestHooks hooks;
	hooks.bypass_input_ordering = true;
	p.test_hooks = &hooks;
	D2_TILED_NEED(tiled(shuffled_source, input, p, broken, report));
	CHECK(content_bytes(broken) != content_bytes(expected));
	D2_TILED_NEED(broken.leaf_gaussians.size() == expected.leaf_gaussians.size());
	CHECK(std::memcmp(broken.leaf_gaussians.ptr(), expected.leaf_gaussians.ptr(),
			size_t(expected.leaf_gaussians.size()) * sizeof(Gaussian)) != 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] tiled RAM admission fails closed and can retry") {
	// Fails if a one-byte aggregate budget is ignored, loses its distinct status,
	// leaves stale/partial output, or leaks reservations that prevent a legal retry.
	const Input input = make_input(40960, 1, 12);
	Source source(input, true);
	BakeResult expected, result;
	D2_TILED_NEED(in_core(input, expected));
	result = expected; // cleanup must clear existing output, not just fresh vectors
	TiledBakeParams p = params(8);
	p.limits.max_ram_bytes = 1;
	TiledBakeReport report;
	String error;
	CHECK(TiledAutoTree::build(source, p, result, report, &error) == ERR_OUT_OF_MEMORY);
	CHECK_FALSE(error.is_empty());
	no_output(result);
	D2_TILED_NEED(tiled(source, input, params(8), result, report));
	same_content(expected, result);
}

struct StopControl {
	std::atomic<uint64_t> now{ 0 };
	std::atomic<bool> cancelled{ false };
	std::atomic<uint32_t> completed{ 0 };
	uint64_t deadline = 0; // immutable while workers run, in the real OS tick domain
	bool expire = false; // configured before build; immutable while workers run

	static uint64_t clock(void *p_data) { return static_cast<StopControl *>(p_data)->now.load(); }
	static void after_tile(void *p_data) {
		auto &s = *static_cast<StopControl *>(p_data);
		s.completed.fetch_add(1);
		if (s.expire) {
			s.now.store(s.deadline);
		} else {
			s.cancelled.store(true);
		}
	}
};

inline void stop_test(bool p_expire) {
	const Input input = make_input(163840, 8);
	BakeResult expected;
	D2_TILED_NEED(in_core(input, expected));
	for (bool during_work : { false, true }) {
		CAPTURE(during_work);
		Source source(input, true);
		StopControl stop;
		stop.expire = p_expire;
		stop.now.store(OS::get_singleton()->get_ticks_usec());
		stop.deadline = stop.now.load() + 5400ull * 1000000ull;
		TiledBakeTestHooks hooks;
		hooks.userdata = &stop;
		hooks.clock_usec = StopControl::clock;
		hooks.tile_completed = during_work ? StopControl::after_tile : nullptr;
		TiledBakeParams p = params(2);
		p.test_hooks = &hooks;
		p.limits.deadline_usec = stop.deadline;
		p.limits.cancel = &stop.cancelled;
		if (!during_work) {
			if (p_expire) {
				stop.now.store(stop.deadline);
			} else {
				stop.cancelled.store(true);
			}
		}
		BakeResult result = expected;
		TiledBakeReport report;
		String error;
		CHECK(TiledAutoTree::build(source, p, result, report, &error) == (p_expire ? ERR_TIMEOUT : ERR_SKIP));
		CHECK_FALSE(error.is_empty());
		no_output(result);
		if (during_work) {
			CHECK(stop.completed.load() > 0u);
		} else {
			CHECK(stop.completed.load() == 0u);
		}
		// Remove the forcing condition: proves the legal route still works and
		// that stopped workers/reservations do not poison the next tiled job.
		D2_TILED_NEED(tiled(source, input, params(2), result, report));
		same_content(expected, result);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] tiled deadline fails closed before and during work") {
	// Fails if deadline equality is accepted, checks only run at entry, status
	// collapses into generic failure, partial output escapes, or retry stays blocked.
	stop_test(true);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] tiled cancellation fails closed before and during work") {
	// Fails if cancellation is ignored after a tile, differs across workers,
	// returns the deadline/RAM status, publishes partial output, or prevents retry.
	stop_test(false);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] bounded histogram coarsening preserves in-core bytes") {
	const Input input = make_input(40960, 8, 12);
	Source source(input, true);
	BakeResult expected, actual;
	if (!in_core(input, expected)) {
		FAIL("D2-6 histogram reference bake failed");
		return;
	}
	TiledBakeTestHooks hooks;
	hooks.histogram_max_bins = 4;
	TiledBakeParams options = params(2);
	options.max_tile_splats = 16385;
	options.test_hooks = &hooks;
	TiledBakeReport report;
	if (!tiled(source, input, options, actual, report)) {
		FAIL("D2-6 coarsened histogram build failed");
		return;
	}
	// This checks the forcing hook's wiring as well as output identity: removing
	// the cap or silently retaining the fine histogram cannot make this green.
	CHECK(report.histogram_passes > 1u);
	CHECK(report.tiles_baked > 1u);
	CHECK(source.reads.load() > source.get_info().chunk_count);
	same_content(expected, actual);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][D2] bounded Morton rank cuts preserve coincident and finest-cell bytes") {
	uint32_t count = 9u * kMaxNodeSplats + 17u;
	bool coincident = true;
	SUBCASE("coincident multilevel index groups") {
		coincident = true;
	}
	SUBCASE("finest cell with nonidentical Morton keys and global-index ties") {
		count = 2u * kMaxNodeSplats + 3u;
		coincident = false;
	}
	Input input = make_input(count, 1);
	for (uint32_t i = 0; i < input.gaussians.size(); ++i) {
		input.gaussians[i].position = coincident ? Vector3(2, 2, 2) :
				Vector3(0.000001f + float(i % 13u) * 0.0000005f,
						0.000001f + float((i / 13u) % 11u) * 0.0000005f, 0.000001f);
	}
	BakeResult expected;
	if (!in_core(input, expected)) {
		FAIL("D2-6 index-cut reference bake failed");
		return;
	}
	for (uint32_t workers : { 1u, 4u }) {
		Source source(input, true);
		TiledBakeParams options = params(workers);
		options.max_tile_splats = 16385;
		BakeResult actual;
		TiledBakeReport report;
		if (!tiled(source, input, options, actual, report)) {
			FAIL("D2-6 bounded index-cut build failed");
			return;
		}
		CHECK(report.tiles_baked == (count + kMaxNodeSplats - 1u) / kMaxNodeSplats);
		bool indexed = false;
		for (const GaussianSplatHlodNode &node : actual.tree.nodes) {
			indexed = indexed || (node.flags & kNodeFlagSplitByIndex) != 0u;
		}
		CHECK(indexed);
		same_content(expected, actual);
	}
}

#undef D2_TILED_NEED

} // namespace TestHlodD2TiledBake
