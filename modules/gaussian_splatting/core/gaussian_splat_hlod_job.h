#ifndef GAUSSIAN_SPLAT_HLOD_JOB_H
#define GAUSSIAN_SPLAT_HLOD_JOB_H

// Single synchronous offline bake only. This is not a scheduler or a general allocator.
// The CLI owns process/host/disk admission; this scope owns the bake's allocations and work.
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/variant/dictionary.h"
#include "gaussian_splat_hlod_quality.h"

#include <atomic>
#include <cmath>

namespace gs_hlod {

struct OfflineJob {
	uint64_t input = 0, prepared = 0, nodes = 0, scratch = 0, output = 0;
	uint64_t deadline = 0, reserved = 0;
	uint64_t admitted_count = 0, admitted_high = 0;
	mutable Error stopped = OK;
	String cancel_path;
	QualityResourceLimits metric;

	static bool positive_integer(const Dictionary &p_options, const char *p_key, uint64_t &r_value, bool p_zero = false)
	{
		if (!p_options.has(p_key)) {
			return false;
		}
		const Variant v = p_options[p_key];
		if (v.get_type() != Variant::INT && v.get_type() != Variant::FLOAT) {
			return false;
		}
		const double n = v;
		if (!std::isfinite(n) || n < (p_zero ? 0.0 : 1.0) || n > 9007199254740991.0 || std::floor(n) != n) {
			return false;
		}
		r_value = uint64_t(n);
		return true;
	}

	bool configure(const Dictionary &p_options)
	{
		uint64_t wall = 0, batch = 0, drain = 0, ram = 0, source = 0, host = 0, disk = 0;
		uint64_t active = 0, pending = 0, workers = 0;
		if (!positive_integer(p_options, "max_input_splats", input) ||
		    !positive_integer(p_options, "max_prepared_splats", prepared) ||
		    !positive_integer(p_options, "max_nodes", nodes) ||
		    !positive_integer(p_options, "max_scratch_bytes", scratch) ||
		    !positive_integer(p_options, "max_output_bytes", output) ||
		    !positive_integer(p_options, "job_wall_seconds", wall) ||
		    !positive_integer(p_options, "batch_wall_seconds", batch) ||
		    !positive_integer(p_options, "cancel_and_drain_seconds", drain) ||
		    !positive_integer(p_options, "process_private_bytes", ram) ||
		    !positive_integer(p_options, "max_source_bytes", source) ||
		    !positive_integer(p_options, "minimum_free_host_physical_and_commit_bytes", host) ||
		    !positive_integer(p_options, "minimum_free_output_volume_bytes", disk) ||
		    !positive_integer(p_options, "concurrent_jobs", active) ||
		    !positive_integer(p_options, "pending_jobs", pending, true) ||
		    !positive_integer(p_options, "bake_workers", workers) ||
		    !positive_integer(p_options, "image_payload_bytes", metric.image_payload_bytes) ||
		    !positive_integer(p_options, "metric_scratch_peak_bytes", metric.scratch_peak_bytes) ||
		    !positive_integer(p_options, "node_work", metric.node_work) ||
		    !positive_integer(p_options, "bake_work", metric.bake_work)) {
			return false;
		}
		const QualityResourceLimits fixed;
		if (active != 1 || pending != 0 || workers != 1 || input > UINT32_MAX || prepared > UINT32_MAX ||
		    nodes > UINT32_MAX || prepared < input || scratch > ram || output > SIZE_MAX || wall > batch ||
		    wall > UINT64_MAX / 1000000u || metric.image_payload_bytes > fixed.image_payload_bytes ||
		    metric.scratch_peak_bytes > fixed.scratch_peak_bytes || metric.node_work > fixed.node_work ||
		    metric.bake_work > fixed.bake_work || !p_options.has("cancel_path") ||
		    p_options["cancel_path"].get_type() != Variant::STRING) {
			return false;
		}
		cancel_path = p_options["cancel_path"];
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		if (cancel_path.is_empty() || wall * 1000000u > UINT64_MAX - now) {
			return false;
		}
		deadline = now + wall * 1000000u;
		return true;
	}

	// Reserve a simultaneous-live upper bound BEFORE snapshot/allocation. Frozen default
	// reduction=4 gives sum(interior counts) <= ceil(input/3) + number of interior nodes:
	// 4 I <= N + I + 3 J. Every selected candidate has the merged candidate's count.
	// LocalVector growth is <=1.5x; CowData rounds to <2x. The terms include old+new
	// owners during growth/probing, not just logical payload sizes. Metadata is disallowed.
	bool reserve(uint64_t p_count, uint64_t p_high, uint64_t p_old_interior_bytes)
	{
		if (!p_count || p_count > input || p_high > 21u || p_count > prepared) {
			return false;
		}
		const uint64_t payload = sizeof(Gaussian) + p_high * sizeof(Vector3);
		const uint64_t interiors = MIN(prepared - p_count, (p_count + 2u) / 3u + nodes);
		const uint64_t children = MIN(p_count, uint64_t(kMaxChildren) * kMaxNodeSplats);
		const uint64_t candidate = MIN(children, uint64_t(kMaxNodeSplats));
		uint64_t remaining = scratch;
		auto charge = [&](uint64_t count, uint64_t bytes) { return quality_charge_product(remaining, count, bytes); };
		if (!charge(3u * p_count, payload) ||                    // source, snapshot, reordered leaves
		    !charge(3u * interiors, payload) ||                  // retained rounded candidates + flattened interior
		    !charge(6u * children + 10u * candidate, payload) || // merge/selection owners and growth
		    !charge(children, 128u) ||                           // merge buckets, keys, members, orders and growth
		    !charge(p_count, 64u) ||                  // permutations, index splits, importance, leaf chunk indices
		    !charge(nodes, 2048u) ||                  // node/build/DFS/BFS tables, child vectors, validation, headers
		    !charge(metric.scratch_peak_bytes, 1u) || // existing old+new metric ledger
		    !charge(p_old_interior_bytes, 1u) || !charge(uint64_t(1) << 20u, 1u)) {
			return false;
		}
		reserved = scratch - remaining;
		// Strict resident reload overlaps the source and candidate with all/leaf/final
		// loader copies. For larger caller-supplied caps it can exceed the bake peak.
		uint64_t reload_remaining = scratch;
		if (!quality_charge_product(reload_remaining, 5u * p_count + 3u * interiors, payload) ||
		    !quality_charge_product(reload_remaining, nodes, 2048u) ||
		    !quality_charge_product(reload_remaining, p_count, 64u) ||
		    !quality_charge_product(reload_remaining, p_old_interior_bytes, 1u) ||
		    !quality_charge_product(reload_remaining, uint64_t(1) << 20u, 1u)) {
			return false;
		}
		reserved = MAX(reserved, scratch - reload_remaining);
		admitted_count = p_count;
		admitted_high = p_high;
		return true;
	}

	Error check() const
	{
		if (stopped != OK) {
			return stopped;
		}
		if (OS::get_singleton()->get_ticks_usec() >= deadline) {
			stopped = ERR_TIMEOUT;
		} else if (FileAccess::exists(cancel_path)) {
			stopped = ERR_SKIP;
		}
		return stopped;
	}
};

inline thread_local OfflineJob *active_offline_job = nullptr;
inline std::atomic<bool> offline_job_busy{false};
inline Error offline_job_check()
{
	return active_offline_job ? active_offline_job->check() : OK;
}

struct OfflineJobScope {
	bool acquired = false;
	explicit OfflineJobScope(OfflineJob &p_job)
	{
		bool expected = false;
		acquired = offline_job_busy.compare_exchange_strong(expected, true);
		if (acquired) {
			active_offline_job = &p_job;
		}
	}
	~OfflineJobScope()
	{
		if (acquired) {
			active_offline_job = nullptr;
			offline_job_busy = false;
		}
	}
};

} // namespace gs_hlod
#endif
