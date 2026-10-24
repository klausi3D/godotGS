/**
 * tile_render_debug_stats.cpp — TileRenderer::TileRendererDebugStats method implementations.
 *
 * Companion .cpp for tile_renderer.h / tile_render_stages.h.
 * Contains all debug statistics, GPU counter readback, overflow tracking,
 * and splat audit methods for the TileRendererDebugStats inner class.
 *
 * Pattern 11 (RAII for RIDs): debug_counter_buffer, overflow_statistics_buffer,
 * and debug_splat_audit_buffer are owned RIDs created/freed by this stage.
 */

#include "tile_renderer.h"
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_method_pointer.h"
#include "servers/rendering/rendering_device.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

#include "gpu_debug_utils.h"
#include "gpu_performance_monitor.h"
#include "gpu_sorter.h"
#include "gpu_sorting_config.h"
#include "quantization_config.h"
#include "resource_owner_mismatch_contract.h"
#include "../logger/gs_logger.h"
#include "gaussian_gpu_layout.h"
#include "pipeline_io_contracts.h"
#include "shader_compilation_helper.h"
#include "sh_config.h"
#include "tile_prefix_scan_utils.h"
#include "../interfaces/sync_policy.h"
#include "../shaders/tile_resolve.glsl.gen.h"

using GaussianSplatting::PassColors;
using GaussianSplatting::ScopedGpuMarker;
using GaussianSplatting::ScopedGpuMarkerEx;

#ifdef TESTS_ENABLED
#include "core/io/dir_access.h"
#include "core/config/engine.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include <atomic>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace {

static constexpr uint32_t kDebugSplatAuditInvalidIndex = 0xFFFFFFFFu;
static constexpr uint32_t kSplatAuditFlagProjected = 1u << 0u;
static constexpr uint32_t kSplatAuditFlagInViewport = 1u << 1u;
static constexpr uint32_t kSplatAuditFlagIterated = 1u << 2u;
static constexpr uint32_t kSplatAuditFlagContributed = 1u << 3u;
static constexpr uint32_t kSplatAuditFlagAlphaSkipped = 1u << 4u;

struct DebugSplatAuditHeader {
	uint32_t enabled = 0;
	uint32_t sample_count = 0;
	uint32_t frame_id = 0;
	uint32_t reserved = 0;
};

struct DebugSplatAuditEntry {
	uint32_t global_idx = kDebugSplatAuditInvalidIndex;
	uint32_t expected_x = 0;
	uint32_t expected_y = 0;
	uint32_t flags = 0;
};

struct DebugSplatAuditBuffer {
	DebugSplatAuditHeader header;
	DebugSplatAuditEntry entries[TileRenderer::DEBUG_SPLAT_AUDIT_MAX_SAMPLES];
};

static_assert(sizeof(DebugSplatAuditHeader) == 16, "DebugSplatAuditHeader must be 16 bytes");
static_assert(sizeof(DebugSplatAuditEntry) == 16, "DebugSplatAuditEntry must be 16 bytes");
static_assert(sizeof(DebugSplatAuditBuffer) == sizeof(DebugSplatAuditHeader) +
		sizeof(DebugSplatAuditEntry) * TileRenderer::DEBUG_SPLAT_AUDIT_MAX_SAMPLES,
		"DebugSplatAuditBuffer size mismatch");

} // namespace

void TileRenderer::TileRendererDebugStats::on_debug_counters_readback(const Vector<uint8_t> &p_data) {
    debug_counter_readback.pending = false;
    const size_t debug_bytes = sizeof(DebugCounterSnapshot);
    if ((size_t)p_data.size() < debug_bytes) {
        return;
    }
    const DebugCounterSnapshot *ptr = reinterpret_cast<const DebugCounterSnapshot *>(p_data.ptr());
    cached_debug_counters = *ptr;
    cached_debug_frame_serial = debug_counter_readback.requested_frame_serial;
}

void TileRenderer::TileRendererDebugStats::on_overflow_stats_readback(const Vector<uint8_t> &p_data) {
    overflow_stats_readback.pending = false;
    const size_t overflow_bytes = sizeof(OverflowStatsSnapshot);
    if ((size_t)p_data.size() < overflow_bytes) {
        return;
    }
    const OverflowStatsSnapshot *ptr = reinterpret_cast<const OverflowStatsSnapshot *>(p_data.ptr());
    cached_overflow_stats = *ptr;
    cached_overflow_frame_serial = overflow_stats_readback.requested_frame_serial;
}

void TileRenderer::TileRendererDebugStats::on_splat_audit_readback(const Vector<uint8_t> &p_data) {
    splat_audit_readback.pending = false;
    const size_t audit_bytes = sizeof(DebugSplatAuditBuffer);
    if ((size_t)p_data.size() < audit_bytes) {
        return;
    }

    const DebugSplatAuditBuffer *buffer = reinterpret_cast<const DebugSplatAuditBuffer *>(p_data.ptr());
    SplatAuditSnapshot snapshot;
    snapshot.frame_serial = splat_audit_readback.requested_frame_serial;
    snapshot.sample_count = buffer->header.sample_count;
    snapshot.valid = (buffer->header.enabled != 0u && buffer->header.sample_count > 0u);

    const uint32_t sample_count = MIN<uint32_t>(buffer->header.sample_count,
            TileRenderer::DEBUG_SPLAT_AUDIT_MAX_SAMPLES);
    for (uint32_t i = 0; i < sample_count; ++i) {
        const DebugSplatAuditEntry &entry = buffer->entries[i];
        const uint32_t flags = entry.flags;
        if ((flags & kSplatAuditFlagProjected) != 0u) {
            snapshot.projected_count++;
        }
        if ((flags & kSplatAuditFlagInViewport) != 0u) {
            snapshot.in_viewport_count++;
        }
        if ((flags & kSplatAuditFlagIterated) != 0u) {
            snapshot.iterated_count++;
        }
        if ((flags & kSplatAuditFlagContributed) != 0u) {
            snapshot.contributed_count++;
        }
        if ((flags & kSplatAuditFlagAlphaSkipped) != 0u) {
            snapshot.alpha_skipped_count++;
        }

        const bool missing_iterated = (flags & kSplatAuditFlagProjected) != 0u &&
                (flags & kSplatAuditFlagInViewport) != 0u &&
                (flags & kSplatAuditFlagIterated) == 0u;
        if (missing_iterated) {
            snapshot.missing_iterated_count++;
        }

        const bool missing_contrib = (flags & kSplatAuditFlagIterated) != 0u &&
                (flags & kSplatAuditFlagContributed) == 0u &&
                (flags & kSplatAuditFlagAlphaSkipped) == 0u;
        if (missing_contrib) {
            snapshot.missing_contrib_count++;
        }

        if ((missing_iterated || missing_contrib) && snapshot.first_mismatch_flags == 0u) {
            snapshot.first_mismatch_global_idx = entry.global_idx;
            snapshot.first_mismatch_expected_x = entry.expected_x;
            snapshot.first_mismatch_expected_y = entry.expected_y;
            snapshot.first_mismatch_flags = flags;
        }
    }

    cached_splat_audit_snapshot = snapshot;
    cached_splat_audit_frame_serial = splat_audit_readback.requested_frame_serial;
}

void TileRenderer::TileRendererDebugStats::poll_overflow_drop_signal(RenderingDevice *p_device, uint64_t p_frame_serial) {
    // C4b / exit criterion G4 ("no silent degradation"): always-on production readback of the
    // resident OverflowStats SSBO (binding 3). The full-buffer overflow readback
    // (get_overflow_stats / dump_gpu_debug_counters) is debug-gated, so a pure production frame
    // would otherwise never learn that a drop happened. This is a tiny async GPU->CPU copy
    // (never a synchronous stall), issued only while no prior read is in flight.
    //
    // READ-AND-RESET (#1137 review): the copy is immediately followed by a clear of the signal
    // word, recorded in the same command stream. RenderingDevice records the copy into the draw
    // graph at this call (add_buffer_get_data on the buffer's tracker) and orders the clear after
    // it, so every bit any writer ORs into the word is seen by exactly one copy: bits set before
    // the copy are in this snapshot, bits set after the clear are left for the next one. The
    // word is otherwise STICKY (clear_counters never touches it), so skipping a poll while a
    // read is pending loses nothing either. The earlier design re-armed by full-clearing the
    // word at the next frame start, after the callback; a drop on ANOTHER channel that landed
    // between the copy and that clear was erased unseen, losing its warning and counter.
    if (!p_device || !overflow_statistics_buffer.is_valid()) {
        return;
    }
    if (p_frame_serial == 0 || overflow_signal_readback.pending) {
        return;
    }
    // #1137: read the whole 88-byte snapshot rather than only the signal word, so the same
    // production readback also yields the sampled frame's drop counts (overflow_splats_clamped /
    // overflow_tile_count; per-frame, cleared by clear_counters). The copy is recorded after the
    // raster pass, so it sees both the binning and the raster-cap writers of this frame.
    overflow_signal_readback.requested_frame_serial = p_frame_serial;
    Callable callback = callable_mp(&owner, &TileRenderer::_on_overflow_signal_readback);
    Error err = p_device->buffer_get_data_async(overflow_statistics_buffer, callback, 0, sizeof(OverflowStatsSnapshot));
    if (err == OK) {
        overflow_signal_readback.pending = true;
        const uint32_t signal_offset = static_cast<uint32_t>(offsetof(OverflowStatsSnapshot, overflow_drop_signal));
        p_device->buffer_clear(overflow_statistics_buffer, signal_offset, sizeof(uint32_t));
    }
}

void TileRenderer::TileRendererDebugStats::on_overflow_signal_readback(const Vector<uint8_t> &p_data) {
    overflow_signal_readback.pending = false;
    if ((size_t)p_data.size() < sizeof(OverflowStatsSnapshot)) {
        return;
    }
    OverflowStatsSnapshot sample;
    std::memcpy(&sample, p_data.ptr(), sizeof(OverflowStatsSnapshot));
    const uint32_t drop_signal = sample.overflow_drop_signal;
#ifdef TESTS_ENABLED
	if (membership_enabled() && sample.overflow_splats_clamped != 0u && !membership_stop_requested) {
		membership_stop_requested = true;
		membership_stop_sample_frame = overflow_signal_readback.requested_frame_serial;
		membership_first_loss_sample["sample_frame"] = String::num_uint64(membership_stop_sample_frame);
		membership_first_loss_sample["renderer_id"] = String::num_uint64(uint64_t(owner.get_instance_id()));
		membership_first_loss_sample["process_id"] = OS::get_singleton()->get_process_id();
		membership_first_loss_sample["device_generation"] = String::num_uint64(membership_owner.device_id);
		membership_first_loss_sample["receipt_ticks_us"] = String::num_uint64(OS::get_singleton()->get_ticks_usec());
		membership_first_loss_sample["sampled_dropped_records"] = sample.overflow_splats_clamped;
		membership_first_loss_sample["raw_overflow_bytes"] = p_data;
		print_line("[RECORD-LOSS] FIRST_SAMPLED_LOSS " + JSON::stringify(membership_first_loss_sample));
	}
#endif
	owner.diagnostics.sampled_dropped_records = sample.overflow_splats_clamped;
    owner.diagnostics.sampled_raster_truncated_tiles = sample.overflow_tile_count;
    owner.diagnostics.sampled_drop_frame_serial = overflow_signal_readback.requested_frame_serial;
    if ((drop_signal & GaussianSplatting::OVERFLOW_DROP_SIGNAL_RASTER_TILE_CAP) != 0u) {
        // #1137: a rasterizer truncated a tile at GS_MAX_RASTER_SPLATS_PER_TILE. Records are
        // depth-sorted, so the farthest content of the densest tiles is missing: tile-shaped
        // holes in far views. Previously only counted on the debug-gated path (silent).
        // Warned once per renderer (on its first event), not once per process, so every
        // renderer that loses content says so.
        if (owner.diagnostics.raster_tile_cap_drop_events == 0u) {
            WARN_PRINT(vformat("[TileRenderer] Per-tile raster cap reached: a %dx%d px tile held more than "
                    "%d overlap records, and the records past the cap (the farthest ones) were not drawn "
                    "(sampled frame: %d tiles truncated by the rasterizer, %d records dropped in all). Dense far views show tile-shaped holes. "
                    "Raise rendering/gaussian_splatting/gpu_sorting/max_raster_splats_per_tile (0 = the "
                    "gpu_preset's value, maximum 65536) or reduce splat density. Shown once per renderer; "
                    "see raster_tile_cap_drop_events in get_overflow_stats() for the running total.",
                    owner.config_state.tile_size, owner.config_state.tile_size,
                    int(TileRenderer::_get_effective_raster_tile_cap()),
                    int(sample.overflow_tile_count), int(sample.overflow_splats_clamped)));
        }
        owner.diagnostics.raster_tile_cap_drop_events++;
    }
    if ((drop_signal & ~GaussianSplatting::OVERFLOW_DROP_SIGNAL_RASTER_TILE_CAP) != 0u) {
        // The binning channel, or any bit this build does not know (fail loud, not silent).
        // G4: overlap-record drops are a real, image-affecting degradation (some splats are
        // not rendered). Surface it loudly once and count it always, instead of dropping
        // silently.
        WARN_PRINT_ONCE("[TileRenderer] Overlap-record overflow: the tile-binning pass dropped "
                "overlap records (per-tile capacity or the global overlap-record budget was "
                "exhausted); some splats are not being rendered. Increase the overlap-record "
                "budget or reduce splat density. Shown once; see the overflow_drop_events "
                "counter for the running total.");
    }
    if (drop_signal != 0u) {
        // overflow_drop_events counts CPU read-intervals in which at least one drop occurred on
        // any channel (the signal is sticky between reads, so this is reliably non-zero whenever
        // drops happen -- it is NOT a per-frame count; the warning is the primary signal). Each
        // interval is counted exactly once because poll reset the word right after its copy.
        owner.diagnostics.overflow_drop_events++;
    }
}

#ifdef TESTS_ENABLED
// This opt-in diagnostic is render-thread owned. The separate control UBO is
// disabled on subsequent frames; the payload stays untouched until its callback.
namespace Membership = GaussianSplatting::TileMembershipCapture;

// Process-wide accounting includes reservations, so parallel renderer owners
// cannot each consume a separate 64MiB allowance. No GPU synchronization involved.
static std::atomic<uint64_t> all_tiles_evidence_usage{0};

bool TileRenderer::TileRendererDebugStats::membership_all_tiles_enabled() const
{
	return OS::get_singleton()->get_environment("GS_RECORD_LOSS_CAPTURE") == "1";
}

bool TileRenderer::TileRendererDebugStats::membership_enabled() const
{
	return membership_all_tiles_enabled();
}

void TileRenderer::TileRendererDebugStats::membership_release_budget()
{
	if (membership_budget_reserved != 0) {
		all_tiles_evidence_usage.fetch_sub(membership_budget_reserved);
		membership_budget_reserved = 0;
	}
}

void TileRenderer::TileRendererDebugStats::membership_problem(const String &p_reason)
{
	if (membership_error.is_empty()) {
		membership_error = p_reason;
	}
	print_line("[RECORD-LOSS] INVALID: " + p_reason);
}

bool TileRenderer::TileRendererDebugStats::membership_save(const Vector<uint8_t> &p_data, const String &p_status)
{
	const String directory = OS::get_singleton()->get_environment("GS_RECORD_LOSS_OUTPUT");
	if (directory.is_empty() || !directory.is_absolute_path() || directory.begins_with("res://") ||
	    directory.begins_with("user://") || DirAccess::make_dir_recursive_absolute(directory) != OK) {
		membership_problem("An absolute writable GS_RECORD_LOSS_OUTPUT is required");
		return false;
	}
	const String stem = "renderer-" + String::num_uint64(uint64_t(owner.get_instance_id())) + "-request-" +
	                    String::num_uint64(membership_nonce) + "-" + p_status;
	membership_metadata["raw_file"] = stem + ".bin";
	membership_metadata["raw_bytes"] = p_data.size();
	membership_metadata["candidate_status"] = p_status;
	membership_metadata["error"] = membership_error;
	membership_metadata["count_dispatches"] = membership_count_dispatches;
	membership_metadata["emit_dispatches"] = membership_emit_dispatches;
	membership_metadata["count_completed"] = membership_count_completed;
	membership_metadata["emit_completed"] = membership_emit_completed;
	membership_metadata["raster_completed"] = membership_raster_completed;
	membership_metadata["successful_copy_mask"] = String::num_uint64(membership_copy_mask);
	membership_metadata["required_copy_mask"] = String::num_uint64(Membership::REQUIRED_COPIES);
	membership_metadata["sampled_drop_frame_serial"] = String::num_uint64(owner.diagnostics.sampled_drop_frame_serial);
	membership_metadata["sampled_dropped_records"] = owner.diagnostics.sampled_dropped_records;
	membership_metadata["stop_sample_frame"] = String::num_uint64(membership_stop_sample_frame);
	membership_metadata["first_loss_sample"] = membership_first_loss_sample.duplicate(true);
	membership_metadata["completion_process_frame"] = String::num_uint64(Engine::get_singleton()->get_process_frames());
	// A JSON file alone never proves completion: final I/O can cross the deadline.
	membership_metadata["completion_requires_log_marker"] = true;
	if (membership_all_tiles_enabled()) {
		membership_metadata["armed_frames"] = membership_captures;
		membership_metadata["header_callbacks"] = membership_header_observations;
		membership_metadata["full_callbacks"] = membership_full_observations;
		membership_metadata["invalid_callbacks"] = membership_invalid_observations;
		membership_metadata["completion_elapsed_us"] =
		        String::num_uint64(OS::get_singleton()->get_ticks_usec() - membership_started_us);
		membership_metadata["frame_coverage"] =
		        "All tiles of admitted frames only; mode0 while callbacks pending can miss other production drops";
		if (p_status.begins_with("INCOMPLETE") || p_status == "INVALID" || p_status == "STALE_CALLBACK_INVALID") {
			membership_metadata["termination_reason"] = p_status;
		}
	}
	const String json_text = JSON::stringify(membership_metadata, "\t");
	if (membership_all_tiles_enabled()) {
		const uint64_t metadata_bytes = uint64_t(json_text.utf8().length());
		const uint64_t bytes = uint64_t(p_data.size()) + metadata_bytes;
		if (metadata_bytes > Membership::ALL_METADATA_LIMIT || bytes > membership_budget_reserved) {
			membership_problem("64MiB host storage or per-packet metadata reservation exhausted");
			membership_finished = true;
			return false;
		}
		// Charge conservatively even if an I/O failure leaves a partial file.
		membership_budget_reserved -= bytes;
	}
	if (FileAccess::exists(directory.path_join(stem + ".bin")) ||
	    FileAccess::exists(directory.path_join(stem + ".json"))) {
		membership_problem("Capture target already exists; refusing overwrite");
		return false;
	}
	Ref<FileAccess> raw = FileAccess::open(directory.path_join(stem + ".bin"), FileAccess::WRITE); // GS_CI_ALLOW_RENDER_PATH_FS_WRITE: TESTS_ENABLED default-off capture; bounded evidence reserved before arm.
	if (raw.is_null() || !raw->store_buffer(p_data)) { // GS_CI_ALLOW_RENDER_PATH_FS_WRITE: TESTS_ENABLED explicit capture; bytes reserved before admission.
		membership_problem("Cannot persist raw capture bytes");
		return false;
	}
	raw.unref();
	Ref<FileAccess> json = FileAccess::open(directory.path_join(stem + ".json"), FileAccess::WRITE); // GS_CI_ALLOW_RENDER_PATH_FS_WRITE: TESTS_ENABLED default-off capture; bounded evidence reserved before arm.
	if (json.is_null() || !json->store_string(json_text)) { // GS_CI_ALLOW_RENDER_PATH_FS_WRITE: TESTS_ENABLED opt-in
		                                                    // metadata; all-tile bytes are budgeted above.
		membership_problem("Cannot persist capture metadata");
		return false;
	}
	json.unref();
	const uint64_t completion_limit_us = 25000000u;
	if (membership_started_us != 0 &&
	    OS::get_singleton()->get_ticks_usec() - membership_started_us > completion_limit_us) {
		membership_problem("Completion deadline exceeded, including bounded async drain and evidence I/O");
		print_line("[RECORD-LOSS] INCOMPLETE " + stem);
		return false;
	}
	print_line("[RECORD-LOSS] COMPLETE " + stem + " " + p_status);
	return true;
}

void TileRenderer::TileRendererDebugStats::membership_create(RenderingDevice *p_device)
{
	if (!membership_enabled() || membership_buffer.is_valid()) {
		return;
	}
	membership_owner.set(p_device);
	Vector<uint8_t> control;
	control.resize(sizeof(Membership::Control));
	control.fill(0);
	membership_buffer = p_device->storage_buffer_create(Membership::ALL_BYTES);
	membership_control = p_device->uniform_buffer_create(control.size(), control);
	if (!membership_buffer.is_valid() || !membership_control.is_valid()) {
		membership_problem("Diagnostic buffer allocation failed");
		membership_free(p_device);
		return;
	}
	p_device->set_resource_name(membership_buffer, "GS_Diagnostic_TileMembership_Payload");
	p_device->set_resource_name(membership_control, "GS_Diagnostic_TileMembership_Control");
}

void TileRenderer::TileRendererDebugStats::membership_free(RenderingDevice *p_device)
{
	if (membership_pending || membership_armed) {
		membership_problem("Resource teardown before observation completed");
		membership_save(Vector<uint8_t>(), "INCOMPLETE_TEARDOWN");
		membership_finished = true;
	}
	if (!membership_pending || membership_followup_ready) {
		membership_release_budget(); // No issued callback can return more bytes.
	}
	membership_followup_ready = false;
	membership_pending = false;
	membership_armed = false;
	++membership_nonce; // Never reuse a callback nonce after resource replacement.
	RenderingDevice *device = membership_owner.device ? membership_owner.device : p_device;
	if (device && membership_owner.matches(device)) {
		if (membership_buffer.is_valid() && device->buffer_is_valid(membership_buffer)) {
			device->free(membership_buffer);
		}
		if (membership_control.is_valid() && device->buffer_is_valid(membership_control)) {
			device->free(membership_control);
		}
	}
	membership_buffer = RID();
	membership_control = RID();
	membership_owner.clear();
}

void TileRenderer::TileRendererDebugStats::membership_record_set(RenderingDevice *p_device, RID p_set,
                                                                 const Vector<RD::Uniform> &p_uniforms)
{
	if (!membership_enabled() || !p_set.is_valid()) {
		return;
	}
	// Only the existing cached sets and current transient lighting sets are kept.
	Vector<RID> expired;
	for (const KeyValue<RID, Dictionary> &entry : membership_sets) {
		if (String(entry.value["device_generation"]) != String::num_uint64(p_device->get_device_instance_id()) ||
		    !p_device->uniform_set_is_valid(entry.key)) {
			expired.push_back(entry.key);
		}
	}
	for (RID set : expired) {
		membership_sets.erase(set);
	}
	Dictionary bindings;
	for (const RD::Uniform &uniform : p_uniforms) {
		Vector<String> ids;
		for (uint32_t i = 0; i < uniform.get_id_count(); ++i) {
			ids.push_back(String::num_uint64(uniform.get_id(i).get_id()));
		}
		bindings[String::num_uint64(uniform.binding)] = ids;
	}
	Dictionary record;
	record["set_id"] = String::num_uint64(p_set.get_id());
	record["device_generation"] = String::num_uint64(p_device->get_device_instance_id());
	record["descriptor_generation"] = String::num_uint64(owner.descriptor_generation);
	record["bindings"] = bindings;
	membership_sets.insert(p_set, record);
}

void TileRenderer::TileRendererDebugStats::membership_check_sets(RenderingDevice *p_device, RID p_buffers, RID p_params,
                                                                 RID p_lighting, bool p_count)
{
	Dictionary selected;
	const RID sets[] = {p_buffers, p_params, p_lighting};
	for (int i = 0; i < 3; ++i) {
		const Dictionary *record = membership_sets.getptr(sets[i]);
		if (!record ||
		    String((*record)["device_generation"]) != String::num_uint64(p_device->get_device_instance_id()) ||
		    String((*record)["descriptor_generation"]) != String::num_uint64(owner.descriptor_generation) ||
		    !p_device->uniform_set_is_valid(sets[i])) {
			membership_problem("Actual dispatch uniform set lacks a live creation-time binding record");
			selected[String::num_int64(i)] = String::num_uint64(sets[i].get_id());
		} else {
			selected[String::num_int64(i)] = record->duplicate(true);
		}
	}
	membership_metadata[p_count ? "count_bound_sets" : "emit_bound_sets"] = selected;
	auto check = [&](RID p_set, int p_binding, RID p_expected) {
		const Dictionary *record = membership_sets.getptr(p_set);
		if (!record) {
			return; // Already invalid, retain all other available evidence.
		}
		const Dictionary bindings = (*record)["bindings"];
		const String binding = String::num_int64(p_binding);
		const Vector<String> ids = bindings.get(binding, Vector<String>());
		if (ids.size() != 1 || ids[0] != String::num_uint64(p_expected.get_id())) {
			membership_problem("Bound buffer differs from captured source at binding " + binding);
		}
	};
	check(p_params, 0, owner.uniform_buffers.param_uniform_buffer);
	check(p_params, 1, membership_control);
	check(p_buffers, 0,
	      p_count ? owner.binning_stage.cached_binning_count_gaussian_buffer
	              : owner.binning_stage.cached_binning_gaussian_buffer);
	check(p_buffers, 1,
	      p_count ? owner.binning_stage.cached_binning_count_sorted_indices
	              : owner.binning_stage.cached_binning_sorted_indices);
	check(p_buffers, 3, overflow_statistics_buffer);
	check(p_buffers, 5, owner.global_sort_resources.get_tile_counts_buffer());
	check(p_buffers, 6, debug_counter_buffer);
	check(p_buffers, 10, debug_splat_audit_buffer);
	check(p_buffers, 12, owner.instance_pipeline_buffers.splat_ref_buffer);
	check(p_buffers, 13, owner.instance_pipeline_buffers.instance_buffer);
	check(p_buffers, 15, owner.instance_pipeline_buffers.indirect_count_buffer);
	check(p_buffers, 16, owner.subpixel_history_buffers.subpixel_history_buffer);
	check(p_buffers, 17, owner.subpixel_visibility_buffers.subpixel_visibility_buffer);
	check(p_buffers, 19, owner.global_sort_resources.get_previous_tile_counts_buffer());
	check(p_buffers, 20, owner.instance_pipeline_buffers.instance_grading_buffer);
	check(p_buffers, 21, membership_buffer);
	if (owner.instance_pipeline_buffers.quantization_required) {
		check(p_buffers, 14, owner.instance_pipeline_buffers.quantization_buffer);
		check(p_buffers, 18, owner.instance_pipeline_buffers.chunk_meta_buffer);
	}
	if (!p_count) {
		check(p_buffers, 2, owner.global_sort_resources.keys_buffer);
		check(p_buffers, 4, owner.global_sort_resources.values_buffer);
		check(p_buffers, 7, owner.projection_buffers.projection_buffer);
		check(p_buffers, 8, owner.global_sort_resources.tile_ranges_buffer);
		check(p_buffers, 9, owner.global_sort_resources.indirect_dispatch_buffer);
		if (owner.render_settings.enable_sh_amortization) {
			check(p_buffers, 11, owner.sh_cache_buffers.sh_color_cache);
		}
	}
}

Dictionary TileRenderer::TileRendererDebugStats::membership_identity(bool p_count) const
{
	Dictionary identity;
	identity["device_generation"] = String::num_uint64(membership_owner.device_id);
	identity["descriptor_generation"] = String::num_uint64(owner.descriptor_generation);
	identity["frame_serial"] = String::num_uint64(owner.frame_state.current_frame_serial);
	identity["gaussian"] = String::num_uint64((p_count ? owner.binning_stage.cached_binning_count_gaussian_buffer
	                                                   : owner.binning_stage.cached_binning_gaussian_buffer)
	                                                  .get_id());
	identity["sorted_indices"] = String::num_uint64((p_count ? owner.binning_stage.cached_binning_count_sorted_indices
	                                                         : owner.binning_stage.cached_binning_sorted_indices)
	                                                        .get_id());
	identity["splat_refs"] = String::num_uint64(owner.instance_pipeline_buffers.splat_ref_buffer.get_id());
	identity["instances"] = String::num_uint64(owner.instance_pipeline_buffers.instance_buffer.get_id());
	identity["grading"] = String::num_uint64(owner.instance_pipeline_buffers.instance_grading_buffer.get_id());
	identity["chunk_metadata"] = String::num_uint64(owner.instance_pipeline_buffers.chunk_meta_buffer.get_id());
	identity["quantization"] = String::num_uint64(owner.instance_pipeline_buffers.quantization_buffer.get_id());
	identity["instance_count"] = String::num_uint64(owner.instance_pipeline_buffers.indirect_count_buffer.get_id());
	identity["instance_dispatch"] =
	        String::num_uint64(owner.instance_pipeline_buffers.indirect_dispatch_buffer.get_id());
	identity["history"] = String::num_uint64(owner.subpixel_history_buffers.subpixel_history_buffer.get_id());
	identity["visibility"] = String::num_uint64(owner.subpixel_visibility_buffers.subpixel_visibility_buffer.get_id());
	identity["quantization_bound"] = owner.instance_pipeline_buffers.quantization_required;
	identity["params"] = String::num_uint64(owner.uniform_buffers.param_uniform_buffer.get_id());
	identity["counts_cursor"] = String::num_uint64(owner.global_sort_resources.get_tile_counts_buffer().get_id());
	identity["previous_counts"] =
	        String::num_uint64(owner.global_sort_resources.get_previous_tile_counts_buffer().get_id());
	identity["ranges"] = String::num_uint64(owner.global_sort_resources.tile_ranges_buffer.get_id());
	identity["indirect_header"] = String::num_uint64(owner.global_sort_resources.indirect_dispatch_buffer.get_id());
	identity["keys"] = String::num_uint64(owner.global_sort_resources.keys_buffer.get_id());
	identity["values"] = String::num_uint64(owner.global_sort_resources.values_buffer.get_id());
	return identity;
}

bool TileRenderer::TileRendererDebugStats::membership_copy(RenderingDevice *p_device, RID p_source,
                                                           uint32_t p_source_offset, uint32_t p_word, uint32_t p_bytes)
{
	const uint32_t words = Membership::ALL_WORDS;
	if (!membership_owner.matches(p_device) || !p_source.is_valid() || p_word > words ||
	    p_bytes > (words - p_word) * 4u ||
	    p_device->buffer_copy(p_source, membership_buffer, p_source_offset, p_word * 4u, p_bytes) != OK) {
		membership_problem("Ordered GPU snapshot copy failed");
		return false;
	}
	const uint32_t destinations[] = {Membership::COUNT_PARAMS,
	                                 Membership::COUNTS,
	                                 86u,
	                                 88u,
	                                 Membership::EMIT_PARAMS,
	                                 Membership::ALL_RANGES,
	                                 Membership::ALL_PRE_CURSOR,
	                                 87u,
	                                 91u,
	                                 32u,
	                                 40u,
	                                 Membership::ALL_POST_CURSOR,
	                                 64u,
	                                 96u,
	                                 128u};
	for (uint32_t bit = 0; bit < 15u; ++bit) {
		if (p_word == destinations[bit]) {
			membership_copy_mask |= uint64_t(1u) << bit;
		}
	}
	return true;
}

void TileRenderer::TileRendererDebugStats::membership_before_count(RenderingDevice *p_device, RID p_buffers,
                                                                   RID p_params, RID p_lighting)
{
	if (!membership_enabled()) {
		return;
	}
	if (!membership_owner.matches(p_device) || !membership_buffer.is_valid() || !membership_control.is_valid()) {
		membership_problem("Diagnostic resource/device unavailable");
		membership_finished = true;
		return;
	}
	Membership::Control control;
	if (p_device->buffer_update(membership_control, 0, sizeof(control), &control) != OK) {
		membership_problem("Disabling diagnostic control failed");
		membership_finished = true;
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (membership_started_us != 0 && now - membership_started_us > 25000000u) {
		if (!membership_finished) {
			membership_problem("Twenty-second admission plus five-second evidence drain expired");
			membership_save(Vector<uint8_t>(), "INCOMPLETE_TIMEOUT");
			membership_finished = true;
		}
		return; // Never recycle a pending payload/nonce after timeout.
	}
	if (OS::get_singleton()->get_environment("GS_RECORD_LOSS_STOP") == "1") {
		membership_stop_requested = true;
	}
	if (membership_pending || membership_finished) {
		if (membership_followup_ready && !membership_finished) {
			membership_followup_ready = false;
			membership_request(p_device); // Outside RenderingDevice callback iteration, payload still frozen.
		}
		return;
	}
	if (membership_armed) {
		++membership_count_dispatches;
		membership_problem("Repeated COUNT or missing prior raster; observation invalid");
		if (membership_frame != owner.frame_state.current_frame_serial) {
			membership_request(p_device);
		}
		return;
	}
	if (membership_stop_requested || (membership_started_us != 0 && now - membership_started_us >= 20000000u)) {
		membership_finished = true;
		print_line("[RECORD-LOSS] STOP " + String(membership_stop_requested
		                                                  ? "INCONCLUSIVE_UNCAPTURED_LOSS_OR_EXTERNAL_STOP"
		                                                  : "NOT_REPRODUCED"));
		return;
	}
	const uint32_t tiles = owner.grid_state.total_tiles;
	if (tiles == 0 || tiles > Membership::MAX_TILES) {
		membership_problem("Tile grid outside bounded packet layout");
		membership_finished = true;
		return;
	}
	uint64_t used = all_tiles_evidence_usage.load();
	bool reserved = false;
	while (used <= Membership::ALL_HOST_BUDGET - Membership::ALL_RESERVATION) {
		if (all_tiles_evidence_usage.compare_exchange_weak(used, used + Membership::ALL_RESERVATION)) {
			reserved = true;
			break;
		}
	}
	if (!reserved) {
		membership_problem("Cannot reserve complete observation within 64MiB");
		membership_finished = true;
		return;
	}
	membership_budget_reserved = Membership::ALL_RESERVATION;
	if (membership_started_us == 0) {
		membership_started_us = now;
		print_line("[RECORD-LOSS] BEGIN V1; diagnostic only; default master algorithms");
	}
	membership_frame = owner.frame_state.current_frame_serial;
	++membership_nonce;
	++membership_captures;
	membership_count_dispatches = 1;
	membership_count_completed = 0;
	membership_emit_dispatches = 0;
	membership_emit_completed = 0;
	membership_raster_completed = 0;
	membership_copy_mask = 0;
	membership_error = String();
	membership_metadata = Dictionary();
	membership_metadata["format"] = "GS_RECORD_LOSS_V1";
	membership_metadata["renderer_id"] = String::num_uint64(uint64_t(owner.get_instance_id()));
	membership_metadata["process_id"] = OS::get_singleton()->get_process_id();
	membership_metadata["frame_serial"] = String::num_uint64(membership_frame);
	membership_metadata["nonce"] = String::num_uint64(membership_nonce);
	membership_metadata["captured_ticks_us"] = String::num_uint64(now);
	membership_metadata["captured_process_frame"] = String::num_uint64(Engine::get_singleton()->get_process_frames());
	membership_metadata["total_tiles"] = tiles;
	membership_metadata["full_packet_bytes"] = Membership::ALL_BYTES;
	membership_metadata["header_words"] = Membership::HEADER_WORDS;
	membership_metadata["count_word"] = Membership::COUNTS;
	membership_metadata["range_word"] = Membership::ALL_RANGES;
	membership_metadata["pre_cursor_word"] = Membership::ALL_PRE_CURSOR;
	membership_metadata["post_cursor_word"] = Membership::ALL_POST_CURSOR;
	membership_metadata["attempts_word"] = Membership::ALL_ATTEMPTS;
	membership_metadata["local_drops_word"] = Membership::ALL_LOCAL;
	membership_metadata["global_drops_word"] = Membership::ALL_GLOBAL;
	membership_metadata["forced_control"] = membership_all_control;
	membership_metadata["input_contents_equal_claimed"] = false;
	membership_metadata["count_identity"] = membership_identity(true);
	membership_metadata["emit_defines"] = owner._build_binning_shader_defines();
	membership_metadata["raster_defines"] = owner._build_raster_shader_defines();
	membership_metadata["effective_raster_cap"] = owner._get_effective_raster_tile_cap();
	membership_metadata["host_allocated_capacity"] = owner.global_sort_resources.capacity;
	membership_metadata["host_effective_record_capacity"] = owner._get_effective_overlap_capacity();
	Vector<String> count_defines = owner._build_common_shader_defines(true);
	count_defines.push_back(vformat("#define GS_TILE_SPLAT_CAPACITY %d\n", TileRenderer::MAX_SPLATS_PER_TILE));
	count_defines.push_back("#define GS_TILE_GLOBAL_SORT 1\n");
	count_defines.push_back("#define GS_TILE_GLOBAL_SORT_COUNT_PASS 1\n");
	membership_metadata["count_defines_derived"] = count_defines;
	membership_metadata["count_defines_source"] =
	        "shader_compilation_helper.cpp::_build_binning_count_defines; derived from unchanged builder";
	membership_check_sets(p_device, p_buffers, p_params, p_lighting, true);
	control.mode = 3u;
	control.frame_lo = uint32_t(membership_frame);
	control.frame_hi = uint32_t(membership_frame >> 32u);
	control.total_tiles = tiles;
	control.nonce_lo = uint32_t(membership_nonce);
	control.nonce_hi = uint32_t(membership_nonce >> 32u);
	membership_full_readback = false;
	membership_followup_ready = false;
	membership_metadata["expected_bytes"] = Membership::HEADER_WORDS * 4u;
	uint32_t header[Membership::HEADER_WORDS] = {};
	header[0] = Membership::MAGIC;
	header[1] = 1;
	header[2] = 3;
	header[3] = UINT32_MAX;
	header[4] = control.frame_lo;
	header[5] = control.frame_hi;
	header[6] = control.nonce_lo;
	header[7] = control.nonce_hi;
	header[25] = tiles;
	header[94] = membership_all_control;
	membership_armed = true;
	if (p_device->buffer_clear(membership_buffer, 0, Membership::ALL_BYTES) != OK ||
	    p_device->buffer_update(membership_buffer, 0, sizeof(header), header) != OK ||
	    p_device->buffer_update(membership_control, 0, sizeof(control), &control) != OK) {
		membership_problem("Capture initialization failed");
	}
	membership_copy(p_device, owner.uniform_buffers.param_uniform_buffer, 0, Membership::COUNT_PARAMS,
	                sizeof(TileRenderParamsGPU));
	membership_copy(p_device, owner.instance_pipeline_buffers.indirect_count_buffer, 12u, 86u, 4u);
	membership_copy(p_device, owner.instance_pipeline_buffers.indirect_dispatch_buffer, 0, 88u, 12u);
}

void TileRenderer::TileRendererDebugStats::membership_after_count(RenderingDevice *p_device)
{
	if (!membership_armed || membership_pending) {
		return;
	}
	++membership_count_completed;
	if (membership_count_dispatches == 1u) {
		membership_copy(p_device, owner.global_sort_resources.get_tile_counts_buffer(), 0, Membership::COUNTS,
		                owner.grid_state.total_tiles * 4u);
		if (membership_all_control == 2u) {
			membership_copy(p_device, owner.subpixel_visibility_buffers.subpixel_visibility_buffer, 0, 30u, 4u);
		}
	}
}

void TileRenderer::TileRendererDebugStats::membership_before_emit(RenderingDevice *p_device, RID p_buffers,
                                                                  RID p_params, RID p_lighting)
{
	if (!membership_armed || membership_pending) {
		return;
	}
	++membership_emit_dispatches;
	membership_check_sets(p_device, p_buffers, p_params, p_lighting, false);
	const Dictionary identity = membership_identity(false);
	membership_metadata["emit_identity"] = identity;
	if (Variant(membership_metadata["count_identity"]) != Variant(identity) || !membership_owner.matches(p_device)) {
		membership_problem("COUNT/EMIT resource, device or frame changed");
	}
	if (membership_all_control == 2u) {
		uint32_t visible = 2;
		if (p_device->buffer_update(owner.instance_pipeline_buffers.indirect_count_buffer, 12u, 4u, &visible) != OK ||
		    p_device->buffer_copy(membership_buffer, owner.subpixel_visibility_buffers.subpixel_visibility_buffer,
		                          30u * 4u, 4u, 4u) != OK) {
			membership_problem("Forced COUNT-disagreement input change failed");
		}
		membership_copy(p_device, owner.subpixel_visibility_buffers.subpixel_visibility_buffer, 4u, 31u, 4u);
		membership_metadata["forced_input_changes"] =
		        "COUNT visible1 to EMIT visible2; copy actual visibility decision for identical synthetic splat";
	}
	if (membership_all_control == 4u) {
		const uint32_t limit = 1u;
		if (p_device->buffer_update(owner.global_sort_resources.indirect_dispatch_buffer, 12u, 4u, &limit) != OK) {
			membership_problem("Forced effective GLOBAL limit change failed");
		}
		membership_metadata["forced_input_changes"] = "After real prefix, replace effective indirect element_count2 "
		                                              "with1; allocations and range remain unchanged";
	}
	RID cursor = owner.global_sort_resources.get_tile_counts_buffer();
	if (membership_all_control == 1u) {
		const uint32_t dirty = 1u;
		if (p_device->buffer_update(cursor, 0, 4u, &dirty) != OK) {
			membership_problem("Forced dirty cursor change failed");
		}
	}
	const uint32_t tiles = owner.grid_state.total_tiles;
	membership_copy(p_device, cursor, 0, Membership::ALL_PRE_CURSOR, tiles * 4u);
	membership_copy(p_device, owner.global_sort_resources.tile_ranges_buffer, 0, Membership::ALL_RANGES, tiles * 8u);
	membership_copy(p_device, owner.instance_pipeline_buffers.indirect_count_buffer, 12u, 87u, 4u);
	membership_copy(p_device, owner.instance_pipeline_buffers.indirect_dispatch_buffer, 0, 91u, 12u);
	membership_copy(p_device, owner.global_sort_resources.indirect_dispatch_buffer, 0, 32u, 24u);
	membership_copy(p_device, overflow_statistics_buffer, 0, 40u, sizeof(OverflowStatsSnapshot));
	membership_copy(p_device, owner.uniform_buffers.param_uniform_buffer, 0, Membership::EMIT_PARAMS,
	                sizeof(TileRenderParamsGPU));
}

void TileRenderer::TileRendererDebugStats::membership_after_emit(RenderingDevice *p_device)
{
	if (!membership_armed || membership_pending) {
		return;
	}
	++membership_emit_completed;
	membership_copy(p_device, owner.global_sort_resources.get_tile_counts_buffer(), 0, Membership::ALL_POST_CURSOR,
	                owner.grid_state.total_tiles * 4u);
	membership_copy(p_device, overflow_statistics_buffer, 0, 64u, sizeof(OverflowStatsSnapshot));
	// Full payload is not observable until the actual raster completes on this device/frame.
}

void TileRenderer::TileRendererDebugStats::membership_after_raster(RenderingDevice *p_device, RID p_buffers,
                                                                   RID p_params, bool p_recorded)
{
	if (!membership_armed || membership_pending) {
		return;
	}
	if (!p_recorded || membership_frame != owner.frame_state.current_frame_serial ||
	    !membership_owner.matches(p_device)) {
		membership_problem("Raster absent or frame/device mismatch");
	} else {
		++membership_raster_completed;
	}
	Dictionary sets;
	for (RID set : {p_buffers, p_params}) {
		const Dictionary *record = membership_sets.getptr(set);
		if (!record ||
		    String((*record)["device_generation"]) != String::num_uint64(p_device->get_device_instance_id()) ||
		    String((*record)["descriptor_generation"]) != String::num_uint64(owner.descriptor_generation) ||
		    !p_device->uniform_set_is_valid(set)) {
			membership_problem("Actual raster descriptor lacks live binding record");
		} else {
			sets[String::num_uint64(set.get_id())] = record->duplicate(true);
		}
	}
	auto check = [&](RID set, uint32_t binding, RID expected) {
		const Dictionary *record = membership_sets.getptr(set);
		if (!record) {
			return;
		}
		const Dictionary bindings = (*record)["bindings"];
		const Vector<String> ids = bindings.get(String::num_uint64(binding), Vector<String>());
		if (ids.size() != 1 || ids[0] != String::num_uint64(expected.get_id())) {
			membership_problem("Raster binding differs from observed resource");
		}
	};
	check(p_buffers, 2, owner.global_sort_resources.tile_ranges_buffer);
	check(p_buffers, 3, overflow_statistics_buffer);
	check(p_buffers, 5, owner.global_sort_resources.values_buffer);
	check(p_buffers, 6, owner.global_sort_resources.indirect_dispatch_buffer);
	check(p_params, 0, owner.uniform_buffers.param_uniform_buffer);
	membership_metadata["raster_bound_sets"] = sets;
	membership_metadata["raster_frame_serial"] = String::num_uint64(owner.frame_state.current_frame_serial);
	membership_metadata["raster_process_frame"] = String::num_uint64(Engine::get_singleton()->get_process_frames());
	membership_metadata["raster_device_generation"] = String::num_uint64(p_device->get_device_instance_id());
	membership_copy(p_device, overflow_statistics_buffer, 0, 96u, sizeof(OverflowStatsSnapshot));
	membership_copy(p_device, owner.global_sort_resources.indirect_dispatch_buffer, 0, 128u, 24u);
	membership_request(p_device);
}

void TileRenderer::TileRendererDebugStats::membership_request(RenderingDevice *p_device)
{
	membership_armed = false;
	membership_pending = true;
	const uint32_t bytes = uint32_t(int64_t(membership_metadata["expected_bytes"]));
	const Error error = p_device->buffer_get_data_async(
	        membership_buffer,
	        callable_mp(&owner, &TileRenderer::_on_membership_readback).bind(int64_t(membership_nonce)), 0, bytes);
	if (error != OK) {
		membership_pending = false;
		membership_finished = true;
		membership_problem("Async snapshot request failed");
		membership_save(Vector<uint8_t>(), "INCOMPLETE_REQUEST_FAILED");
		membership_release_budget();
	}
}

void TileRenderer::TileRendererDebugStats::membership_readback(const Vector<uint8_t> &p_data, uint64_t p_nonce)
{
	if (!membership_pending || p_nonce != membership_nonce) {
		membership_problem("Stale callback; raw bytes invalid");
		membership_save(p_data, "STALE_CALLBACK_INVALID");
		membership_finished = true;
		membership_release_budget();
		return;
	}
	membership_pending = false;
	const uint32_t expected = uint32_t(int64_t(membership_metadata["expected_bytes"]));
	auto word = [&](uint32_t i) -> uint32_t {
		uint32_t value = 0;
		memcpy(&value, p_data.ptr() + i * 4u, 4u);
		return value;
	};
	if (p_data.size() != expected || p_data.size() < int(Membership::HEADER_WORDS * 4u)) {
		membership_problem("Short or malformed async payload");
	} else if (word(0) != Membership::MAGIC || word(1) != 1u || word(2) != 3u ||
	           (uint64_t(word(6)) | uint64_t(word(7)) << 32u) != membership_nonce ||
	           (uint64_t(word(4)) | uint64_t(word(5)) << 32u) != membership_frame ||
	           word(25) != uint32_t(int64_t(membership_metadata["total_tiles"])) || word(10) != 0u || word(125) != 1u) {
		membership_problem("Packet identity, counter wrap or actual capacity observation invalid");
	}
	if (!membership_owner.matches(membership_owner.device) || membership_count_dispatches != 1u ||
	    membership_count_completed != 1u || membership_emit_dispatches != 1u || membership_emit_completed != 1u ||
	    membership_raster_completed != 1u || membership_copy_mask != Membership::REQUIRED_COPIES) {
		membership_problem("Missing/repeated stage or successful copy");
	}
	if (membership_full_readback) {
		++membership_full_observations;
	} else {
		++membership_header_observations;
	}
	const bool loss =
	        membership_error.is_empty() && (word(11) != 0u || word(12) != 0u || word(97) != 0u || word(96) != 0u);
	if (!membership_full_readback) {
		const bool need_full =
		        !membership_error.is_empty() || loss || membership_all_control != 0u || membership_stop_requested;
		if (!membership_save(p_data, membership_error.is_empty() ? (loss ? "LOSS_HEADER" : "NO_LOSS_HEADER_ONLY")
		                                                         : "INVALID_HEADER")) {
			membership_finished = true;
			membership_release_budget();
			return;
		}
		if (need_full) {
			membership_full_readback = true;
			membership_metadata["expected_bytes"] = Membership::ALL_BYTES;
			membership_pending = true;
			membership_followup_ready = true;
			return;
		}
		membership_release_budget();
		return;
	}
	if (membership_error.is_empty()) {
		uint64_t local = 0, global = 0;
		for (uint32_t t = 0; t < word(25); ++t) {
			local += word(Membership::ALL_LOCAL + t);
			global += word(Membership::ALL_GLOBAL + t);
		}
		if (local > UINT32_MAX || global > UINT32_MAX || local != word(11) || global != word(12)) {
			membership_problem("Drop planes disagree with fresh aggregate counters");
		}
		if (word(11) != 0 && (word(13) != 2u || word(14) >= word(25) ||
		                      (uint64_t(word(22)) | uint64_t(word(23)) << 32u) != membership_frame)) {
			membership_problem("LOCAL witness is incomplete or from another frame");
		}
	}
	if (!membership_error.is_empty()) {
		++membership_invalid_observations;
	}
	membership_metadata["termination_reason"] =
	        !membership_error.is_empty()
	                ? "INVALID"
	                : (loss ? "COMPLETE_LOSS_FRAME"
	                        : (membership_all_control ? "FORCED_NO_DROP_CONTROL" : "INCONCLUSIVE_UNMATCHED_LOSS"));
	const String status =
	        !membership_error.is_empty() ? "INVALID" : (loss ? "COMPLETE_LOSS_FRAME" : "COMPLETE_NO_LOSS_FRAME");
	const bool saved = membership_save(p_data, status);
	membership_finished = true;
	if (saved && membership_error.is_empty() && membership_all_control != 0u) {
		const uint32_t cap = uint32_t(int64_t(membership_metadata["effective_raster_cap"]));
		const uint32_t n = membership_all_control == 5u ? cap + 1u : (membership_all_control == 4u ? 2u : 1u);
		const uint32_t e = membership_all_control == 2u ? 2u : n;
		const uint32_t local = membership_all_control <= 2u ? 1u : 0u;
		const uint32_t global = membership_all_control == 4u ? 1u : 0u;
		const uint32_t raster = membership_all_control >= 4u ? 1u : 0u;
		membership_control_observed = word(25) == 1u && word(Membership::COUNTS) == n &&
		                              word(Membership::ALL_RANGES) == 0u && word(Membership::ALL_RANGES + 1u) == n &&
		                              word(Membership::ALL_PRE_CURSOR) == (membership_all_control == 1u ? 1u : 0u) &&
		                              word(Membership::ALL_POST_CURSOR) == n && word(Membership::ALL_ATTEMPTS) == e &&
		                              word(Membership::ALL_LOCAL) == local && word(Membership::ALL_GLOBAL) == global &&
		                              word(41) == 0u && word(65) == local + global &&
		                              word(97) == local + global + raster && word(86) == n && word(87) == e &&
		                              word(35) == (membership_all_control == 4u ? 1u : n) && word(131) == word(35) &&
		                              word(118) >= n && word(119) >= n;
	}
	print_line("[RECORD-LOSS] STOP " + String(membership_metadata["termination_reason"]));
	membership_release_budget();
}
#endif // TESTS_ENABLED

void TileRenderer::TileRendererDebugStats::create_buffers(RenderingDevice *p_device) {
	ERR_FAIL_NULL(p_device);

	// Create debug counters buffer (TileDebugCounterSnapshot layout)
	Vector<uint8_t> debug_counter_data;
	debug_counter_data.resize(sizeof(DebugCounterSnapshot));
	debug_counter_data.fill(0);
    debug_counter_buffer = p_device->storage_buffer_create(debug_counter_data.size(), debug_counter_data);

    if (!debug_counter_buffer.is_valid()) {
        GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate debug counter buffer");
        debug_counter_owner.clear();
    } else {
        p_device->set_resource_name(debug_counter_buffer, "GS_TileRenderer_DebugCounterBuffer");
        debug_counter_owner.set(p_device);
    }

    // Create overflow statistics buffer (OverflowStatsSnapshot).
    // Includes overflow counters + sampled rasterizer diagnostics (enabled via debug_show_splat_coverage).
    Vector<uint8_t> stats_data;
    stats_data.resize(sizeof(OverflowStatsSnapshot));
    stats_data.fill(0);
    overflow_statistics_buffer = p_device->storage_buffer_create(stats_data.size(), stats_data);

    if (!overflow_statistics_buffer.is_valid()) {
        GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate overflow statistics buffer");
        overflow_stats_owner.clear();
    } else {
        p_device->set_resource_name(overflow_statistics_buffer, "GS_TileRenderer_OverflowStatsBuffer");
        overflow_stats_owner.set(p_device);
    }

    Vector<uint8_t> audit_data;
    audit_data.resize(sizeof(DebugSplatAuditBuffer));
    audit_data.fill(0);
    debug_splat_audit_buffer = p_device->storage_buffer_create(audit_data.size(), audit_data);
    if (!debug_splat_audit_buffer.is_valid()) {
        GS_LOG_ERROR_DEFAULT("[TileRenderer] Failed to allocate splat audit buffer");
        debug_splat_audit_owner.clear();
    } else {
        p_device->set_resource_name(debug_splat_audit_buffer, "GS_TileRenderer_SplatAuditBuffer");
        debug_splat_audit_owner.set(p_device);
    }

#ifdef TESTS_ENABLED
	membership_create(p_device);
#endif
	owner._invalidate_descriptor_cache();
}

void TileRenderer::TileRendererDebugStats::free_buffers(RenderingDevice *p_device) {
#ifdef TESTS_ENABLED
	membership_free(p_device);
#endif
	if (!p_device) {
		debug_counter_buffer = RID();
		overflow_statistics_buffer = RID();
		debug_splat_audit_buffer = RID();
		debug_counter_owner.clear();
		overflow_stats_owner.clear();
		debug_splat_audit_owner.clear();
		return;
	}

	auto safe_free_debug_buffer = [&](RID &p_rid, BufferOwnership &p_owner) {
		RenderingDevice *owner_device = p_owner.device ? p_owner.device : p_device;
		if (p_rid.is_valid() && owner_device && owner_device->buffer_is_valid(p_rid)) {
			owner_device->free(p_rid);
		}
		p_rid = RID();
		p_owner.clear();
	};

	if (debug_counter_buffer.is_valid()) {
		safe_free_debug_buffer(debug_counter_buffer, debug_counter_owner);
	}
	if (debug_splat_audit_buffer.is_valid()) {
		safe_free_debug_buffer(debug_splat_audit_buffer, debug_splat_audit_owner);
	}
	if (overflow_statistics_buffer.is_valid()) {
		safe_free_debug_buffer(overflow_statistics_buffer, overflow_stats_owner);
	}
}

void TileRenderer::TileRendererDebugStats::clear_counters(RenderingDevice *p_device) {
	if (!p_device) {
		return;
	}
	// Clear debug counters at frame start for per-frame tracking
	if (debug_counter_buffer.is_valid()) {
		p_device->buffer_clear(debug_counter_buffer, 0, sizeof(DebugCounterSnapshot));
	}
	// C4b (G4): the trailing overflow_drop_signal (last uint) is a STICKY drop flag, so clear
	// only the per-frame prefix [0, size - 4) here. The signal word is reset ONLY by
	// poll_overflow_drop_signal, right after the copy that reads it (read-and-reset), so no bit
	// can be cleared before a readback has seen it (#1137 review).
	if (overflow_statistics_buffer.is_valid()) {
		p_device->buffer_clear(overflow_statistics_buffer, 0,
				static_cast<uint32_t>(sizeof(OverflowStatsSnapshot) - sizeof(uint32_t)));
	}
}

void TileRenderer::TileRendererDebugStats::update_splat_audit_buffer(RenderingDevice *p_device, const RenderParams &p_params,
        uint64_t p_frame_serial) {
	if (!p_device || !debug_splat_audit_buffer.is_valid()) {
		return;
	}

	DebugSplatAuditBuffer buffer;
	buffer.header.enabled = p_params.debug_enable_splat_audit ? 1u : 0u;
	buffer.header.sample_count = 0u;
	buffer.header.frame_id = static_cast<uint32_t>(p_frame_serial & 0xFFFFFFFFu);
	buffer.header.reserved = 0u;

	for (uint32_t i = 0; i < TileRenderer::DEBUG_SPLAT_AUDIT_MAX_SAMPLES; ++i) {
		buffer.entries[i].global_idx = kDebugSplatAuditInvalidIndex;
		buffer.entries[i].expected_x = 0;
		buffer.entries[i].expected_y = 0;
		buffer.entries[i].flags = 0;
	}

	if (p_params.debug_enable_splat_audit && p_params.splat_count > 0) {
		uint32_t requested_samples = p_params.debug_splat_audit_sample_count;
		if (requested_samples == 0) {
			requested_samples = TileRenderer::DEBUG_SPLAT_AUDIT_MAX_SAMPLES;
		}
		uint32_t available = MIN<uint32_t>(p_params.splat_count, TileRenderer::DEBUG_SPLAT_AUDIT_MAX_SAMPLES);
		uint32_t sample_count = MIN<uint32_t>(requested_samples, available);
		buffer.header.sample_count = sample_count;

		uint32_t step = MAX<uint32_t>(1u, p_params.splat_count / MAX<uint32_t>(sample_count, 1u));
		uint32_t start = static_cast<uint32_t>((p_frame_serial * 2654435761u) % p_params.splat_count);
		for (uint32_t i = 0; i < sample_count; ++i) {
			uint32_t idx = (start + i * step) % p_params.splat_count;
			buffer.entries[i].global_idx = idx;
		}
	}

	Vector<uint8_t> data;
	// #798: unchecked by the compile-time-constant-count rule (gs_vector_alloc.h).
	// sizeof(DebugSplatAuditBuffer) is fixed by DEBUG_SPLAT_AUDIT_MAX_SAMPLES and never
	// scales with scene or asset data, so the memcpy length cannot exceed a successful
	// resize.
	data.resize(sizeof(DebugSplatAuditBuffer));
	std::memcpy(data.ptrw(), &buffer, sizeof(DebugSplatAuditBuffer));
	p_device->buffer_update(debug_splat_audit_buffer, 0, data.size(), data.ptr());
}

void TileRenderer::TileRendererDebugStats::_log_readback_state_if_needed(uint64_t p_frame_serial, bool p_log_enabled) const {
	// DEBUG: Log readback state every 30 frames
	static int dump_call_counter = 0;
	if (p_log_enabled && (++dump_call_counter % 30 == 1)) {
		GS_LOG_RENDERER_DEBUG(vformat("[DEBUG-READBACK] frame=%d cached_debug=%d cached_overflow=%d pending_debug=%d pending_overflow=%d",
				int(p_frame_serial), int(cached_debug_frame_serial), int(cached_overflow_frame_serial),
				debug_counter_readback.pending ? 1 : 0, overflow_stats_readback.pending ? 1 : 0));
	}
}

void TileRenderer::TileRendererDebugStats::_schedule_debug_readbacks(RenderingDevice *p_device, uint64_t p_frame_serial,
		bool p_has_debug_buffer, bool p_has_overflow_buffer) {
	static constexpr uint64_t kDebugReadbackDelayFrames = 2;
	if (p_has_debug_buffer && p_frame_serial > 0 && (p_frame_serial > cached_debug_frame_serial + kDebugReadbackDelayFrames)) {
		const size_t debug_bytes = sizeof(DebugCounterSnapshot);
		if (!debug_counter_readback.pending) {
			debug_counter_readback.requested_frame_serial = p_frame_serial;
			Callable callback = callable_mp(&owner, &TileRenderer::_on_debug_counters_readback);
			Error err = p_device->buffer_get_data_async(debug_counter_buffer, callback, 0, debug_bytes);
			if (err == OK) {
				debug_counter_readback.pending = true;
			}
		}
	}

	if (p_has_overflow_buffer && p_frame_serial > 0 && (p_frame_serial > cached_overflow_frame_serial + kDebugReadbackDelayFrames)) {
		const size_t overflow_bytes = sizeof(OverflowStatsSnapshot);
		if (!overflow_stats_readback.pending) {
			overflow_stats_readback.requested_frame_serial = p_frame_serial;
			Callable callback = callable_mp(&owner, &TileRenderer::_on_overflow_stats_readback);
			Error err = p_device->buffer_get_data_async(overflow_statistics_buffer, callback, 0, overflow_bytes);
			if (err == OK) {
				overflow_stats_readback.pending = true;
			}
		}
	}
}

void TileRenderer::TileRendererDebugStats::_log_debug_counters(uint64_t p_frame_serial, bool p_log_enabled,
		bool p_has_debug_data, bool p_has_overflow_data) {
	if (!p_log_enabled || !p_has_debug_data) {
		return;
	}
	(void)p_has_overflow_data;

	// GPU counters exposed via Custom Performance Monitors
	// See Editor -> Debugger -> Monitors for real-time statistics
	const DebugCounterSnapshot &snapshot = cached_debug_counters;
	if (owner.render_settings.enable_sh_amortization) {
		const uint32_t hits = snapshot.sh_cache_hits;
		const uint32_t updates = snapshot.sh_cache_updates;
		const uint32_t forced = snapshot.sh_cache_forced_updates;
		const uint32_t total = hits + updates;
		const float hit_rate = total > 0 ? (100.0f * float(hits) / float(total)) : 0.0f;
		GS_LOG_RENDERER_DEBUG(vformat("[SH-CACHE] frame=%d divisor=%u hits=%u updates=%u forced=%u hit_rate=%.1f%%",
				int(p_frame_serial),
				owner.render_settings.sh_amortization_divisor,
				hits,
				updates,
				forced,
				hit_rate));
	}
	// GPU timing exposed via Custom Performance Monitors
	// See Editor -> Debugger -> Monitors for real-time GPU timing graphs

	const uint32_t overlap_records = owner.diagnostics.last_overlap_record_count;
	const bool tighter_bounds = owner.render_settings.enable_tighter_bounds;
	const bool toggled = last_logged_valid && (tighter_bounds != last_logged_tighter_bounds);
	if (toggled) {
		GS_LOG_RENDERER_DEBUG(vformat("[GPU-COUNTERS-BASELINE] frame=%d tighter_bounds=%s tile_extent_reject=%d overlap_records=%u",
				int(last_logged_frame_serial),
				last_logged_tighter_bounds ? "YES" : "no",
				int(last_logged_debug_counters.tile_extent_reject),
				last_logged_overlap_records));
	}
	if (last_logged_valid) {
		const int32_t delta_tile_extent = int32_t(snapshot.tile_extent_reject) - int32_t(last_logged_debug_counters.tile_extent_reject);
		const int32_t delta_overlap = int32_t(overlap_records) - int32_t(last_logged_overlap_records);
		const String toggle_note = toggled ? " (toggled)" : "";
		const float overlap_pct = (last_logged_overlap_records > 0)
				? (100.0f * float(delta_overlap) / float(last_logged_overlap_records))
				: 0.0f;
		const String delta_te_str = (delta_tile_extent >= 0 ? "+" : "") + String::num_int64(delta_tile_extent);
		const String delta_ov_str = (delta_overlap >= 0 ? "+" : "") + String::num_int64(delta_overlap);
		GS_LOG_RENDERER_DEBUG("[GPU-COUNTERS-DELTA] frame=" + itos(p_frame_serial) +
				" tighter_bounds=" + (tighter_bounds ? "YES" : "no") + toggle_note +
				" tile_extent_reject=" + itos(snapshot.tile_extent_reject) + " (delta " + delta_te_str + ")" +
				" overlap_records=" + itos(overlap_records) + " (delta " + delta_ov_str + ", " + String::num(overlap_pct, 1) + "%)");
	} else {
		GS_LOG_RENDERER_DEBUG("[GPU-COUNTERS-DELTA] frame=" + itos(p_frame_serial) +
				" tighter_bounds=" + (tighter_bounds ? "YES" : "no") +
				" tile_extent_reject=" + itos(snapshot.tile_extent_reject) +
				" overlap_records=" + itos(overlap_records));
	}
	last_logged_debug_counters = snapshot;
	last_logged_overlap_records = overlap_records;
	last_logged_tighter_bounds = tighter_bounds;
	last_logged_frame_serial = p_frame_serial;
	last_logged_valid = true;
}

void TileRenderer::TileRendererDebugStats::_log_overflow_stats(uint64_t p_frame_serial, bool p_log_enabled, bool p_has_overflow_data) {
	if (!p_log_enabled || !p_has_overflow_data) {
		return;
	}

	const OverflowStatsSnapshot &stats = cached_overflow_stats;

	GS_LOG_RENDERER_DEBUG(vformat("[GPU-OVERFLOW] frame=%d tiles=%d clamped=%d aggregated=%d",
			int(p_frame_serial),
			stats.overflow_tile_count,
			stats.overflow_splats_clamped,
			stats.overflow_splats_aggregated));

	if (stats.raster_sample_count > 0) {
		float avg_alpha = float(stats.raster_alpha_sum_q10) / float(stats.raster_sample_count) / 1024.0f;
		uint32_t no_contrib_tiles = stats.raster_sample_count > stats.raster_has_depth
				? (stats.raster_sample_count - stats.raster_has_depth)
				: 0;
		GS_LOG_RENDERER_DEBUG(vformat("[GPU-RASTER] samples=%d iter=%d contrib=%d has_depth=%d no_contrib=%d avg_alpha=%.4f rejects:{sorted=%d gaussian=%d idx_mismatch=%d base0=%d nan=%d weight=%d alpha=%d} breaks:{remain=%d final=%d subgroup=%d}",
				stats.raster_sample_count,
				stats.raster_splats_iterated,
				stats.raster_splats_contributed,
				stats.raster_has_depth,
				no_contrib_tiles,
				avg_alpha,
				stats.raster_reject_sorted_idx_oob,
				stats.raster_reject_gaussian_idx_oob,
				stats.raster_reject_index_mismatch,
				stats.raster_reject_base_opacity,
				stats.raster_reject_nan_inf,
				stats.raster_reject_weight,
				stats.raster_reject_alpha,
				stats.raster_break_remaining_alpha,
				stats.raster_break_final_alpha,
				stats.raster_break_subgroup_early_exit));
	}

	// Note: overflow buffer is cleared at frame start via _clear_debug_counters(),
	// not here, so that get_overflow_stats() can read it for auto-tune feedback.
}

void TileRenderer::TileRendererDebugStats::dump_gpu_debug_counters(RenderingDevice *p_device, const RenderParams &p_params,
        uint64_t p_frame_serial) {
	if (!p_params.debug_dump_gpu_counters) {
		return;
	}

	RenderingDevice *device = p_device;
	const bool log_enabled = p_params.debug_enable_gpu_counter_logs;
	_log_readback_state_if_needed(p_frame_serial, log_enabled);
	const bool has_debug_buffer = device && debug_counter_buffer.is_valid();
	const bool has_overflow_buffer = device && overflow_statistics_buffer.is_valid();
	if (!has_debug_buffer && !has_overflow_buffer) {
		return;
	}
	_schedule_debug_readbacks(device, p_frame_serial, has_debug_buffer, has_overflow_buffer);

	const bool has_debug_data = has_debug_buffer && cached_debug_frame_serial > 0;
	const bool has_overflow_data = has_overflow_buffer && cached_overflow_frame_serial > 0;
	if (!has_debug_data && !has_overflow_data) {
		return;
	}
	_log_debug_counters(p_frame_serial, log_enabled, has_debug_data, has_overflow_data);
	_log_overflow_stats(p_frame_serial, log_enabled, has_overflow_data);
}

TileRenderer::DebugCounterSnapshot TileRenderer::TileRendererDebugStats::get_debug_counters(RenderingDevice *p_device,
        uint64_t p_frame_serial) const {
    DebugCounterSnapshot snapshot = cached_debug_counters;
    if (!owner.diagnostics.debug_binning_counters_enabled) {
        return snapshot;
    }
    if (!p_device || !debug_counter_buffer.is_valid()) {
        return snapshot;
    }
    static constexpr uint64_t kDebugReadbackDelayFrames = 2;
    if (p_frame_serial > 0 && (p_frame_serial > cached_debug_frame_serial + kDebugReadbackDelayFrames)) {
        const size_t debug_bytes = sizeof(DebugCounterSnapshot);
        if (!debug_counter_readback.pending) {
            debug_counter_readback.requested_frame_serial = p_frame_serial;
            Callable callback = callable_mp(&owner, &TileRenderer::_on_debug_counters_readback);
            Error err = p_device->buffer_get_data_async(debug_counter_buffer, callback, 0, debug_bytes);
            if (err == OK) {
                debug_counter_readback.pending = true;
            }
        }
    }
    return snapshot;
}

TileRenderer::OverflowStatsSnapshot TileRenderer::TileRendererDebugStats::get_overflow_stats(RenderingDevice *p_device,
        uint64_t p_frame_serial) const {
    OverflowStatsSnapshot stats = cached_overflow_stats;
    if (!p_device || !overflow_statistics_buffer.is_valid()) {
        return stats;
    }
    static constexpr uint64_t kDebugReadbackDelayFrames = 2;
    if (p_frame_serial > 0 && (p_frame_serial > cached_overflow_frame_serial + kDebugReadbackDelayFrames)) {
        const size_t overflow_bytes = sizeof(OverflowStatsSnapshot);
        if (!overflow_stats_readback.pending) {
            overflow_stats_readback.requested_frame_serial = p_frame_serial;
            Callable callback = callable_mp(&owner, &TileRenderer::_on_overflow_stats_readback);
            Error err = p_device->buffer_get_data_async(overflow_statistics_buffer, callback, 0, overflow_bytes);
            if (err == OK) {
                overflow_stats_readback.pending = true;
            }
        }
    }
    return stats;
}

TileRenderer::SplatAuditSnapshot TileRenderer::TileRendererDebugStats::get_splat_audit_snapshot(RenderingDevice *p_device,
        uint64_t p_frame_serial) const {
    SplatAuditSnapshot snapshot = cached_splat_audit_snapshot;
    if (!p_device || !debug_splat_audit_buffer.is_valid()) {
        return snapshot;
    }
    static constexpr uint64_t kDebugReadbackDelayFrames = 2;
    if (p_frame_serial > 0 && (p_frame_serial > cached_splat_audit_frame_serial + kDebugReadbackDelayFrames)) {
        const size_t audit_bytes = sizeof(DebugSplatAuditBuffer);
        if (!splat_audit_readback.pending) {
            splat_audit_readback.requested_frame_serial = p_frame_serial;
            Callable callback = callable_mp(&owner, &TileRenderer::_on_splat_audit_readback);
            Error err = p_device->buffer_get_data_async(debug_splat_audit_buffer, callback, 0, audit_bytes);
            if (err == OK) {
                splat_audit_readback.pending = true;
            }
        }
    }
    return snapshot;
}
