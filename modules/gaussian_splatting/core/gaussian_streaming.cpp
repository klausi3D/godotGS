#include "gaussian_streaming.h"
#include "../io/streaming_chunk_bake.h"
#include "gs_project_settings.h"
#include "residency_budget_controller.h"
#include "streaming_queue_pressure_controller.h"
#include "streaming_telemetry_adapter.h"
#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_method_pointer.h"
#include "core/os/os.h"
#include "core/templates/span.h"
#include "core/templates/hashfuncs.h"
#include "gaussian_splat_manager.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../logger/gs_debug_trace.h"
#include "../renderer/gpu_memory_stream.h"
#include "../renderer/gpu_debug_utils.h"
#include "../renderer/quantization_config.h"
#include "../logger/gs_logger.h"
#include "../logger/startup_trace.h"
#include "../interfaces/sync_policy.h"
#include "../lod/lod_config.h"
#include <cfloat>  // For FLT_MAX
#include <cstdint>
#include <algorithm>
#include <utility>

namespace {

static constexpr uint32_t STREAMING_DEFAULT_MAX_UPLOAD_MB_PER_FRAME = 128;
static constexpr uint32_t STREAMING_DEFAULT_MAX_UPLOAD_MB_PER_SLICE = 16;
static constexpr uint32_t STREAMING_DEFAULT_MAX_UPLOAD_MB_PER_SECOND = 0;
static constexpr uint32_t STREAMING_DEFAULT_VRAM_BUDGET_MB = STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB;
static constexpr uint32_t STREAMING_MAIN_RD_UPLOAD_FRAME_DELAY_SAFETY = 1;

static uint32_t _packed_gaussian_payload_checksum(const Vector<PackedGaussian> &p_data) {
    const int byte_count = int(p_data.size() * sizeof(PackedGaussian));
    const uint8_t empty_byte = 0;
    const void *payload = byte_count > 0 ? static_cast<const void *>(p_data.ptr()) : static_cast<const void *>(&empty_byte);
    return hash_murmur3_buffer(payload, byte_count);
}
static constexpr uint32_t STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM = 4;
static constexpr uint32_t STREAMING_DEFAULT_MAX_CHUNKS_IN_VRAM = 128;

uint64_t _streaming_chunk_slot_bytes() {
    return uint64_t(GaussianStreamingSystem::CHUNK_SIZE) * sizeof(PackedGaussian);
}

uint32_t _streaming_addressable_chunk_limit() {
    const uint64_t slot_bytes = _streaming_chunk_slot_bytes();
    if (slot_bytes == 0) {
        return 0;
    }
    return static_cast<uint32_t>(MIN<uint64_t>(uint64_t(UINT32_MAX) / slot_bytes, uint64_t(UINT32_MAX)));
}

uint32_t _streaming_upload_frame_delay(RenderingDevice *p_device) {
    if (!p_device) {
        return 1 + STREAMING_MAIN_RD_UPLOAD_FRAME_DELAY_SAFETY;
    }
    const uint32_t frame_delay = p_device->get_frame_delay();
    return (frame_delay > 0 ? frame_delay : 1) + STREAMING_MAIN_RD_UPLOAD_FRAME_DELAY_SAFETY;
}

const char *_streaming_upload_completion_mode_name(uint8_t p_mode) {
    switch (p_mode) {
        case GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_LOCAL_RD_SUBMIT_SYNC:
            return "local_rd_submit_sync";
        case GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER:
            return "main_rd_frame_delay_barrier";
        case GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_TIMELINE_UNAVAILABLE_FRAME_DELAY:
            return "timeline_unavailable_frame_delay";
        case GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_TEST_MANUAL:
            return "test_manual";
        default:
            return "none";
    }
}

bool _data_has_uniform_dc_encoding(const Ref<GaussianData> &p_data) {
    if (p_data.is_null() || p_data->get_count() <= 1) {
        return true;
    }

    const GaussianDCEncoding first_encoding = gaussian_get_dc_encoding(p_data->get_gaussian(0).render_meta);
    for (int i = 1; i < p_data->get_count(); i++) {
        if (gaussian_get_dc_encoding(p_data->get_gaussian(i).render_meta) != first_encoding) {
            return false;
        }
    }
    return true;
}

struct StreamingTierCapPolicy {
    String tier_preset = "custom";
    bool active = false;
    uint32_t upload_mb_per_frame = 0;
    uint32_t upload_mb_per_slice = 0;
    uint32_t upload_mb_per_second = 0;
    uint32_t vram_budget_mb = 0;
    uint32_t min_chunks_in_vram = 0;
    uint32_t max_chunks_in_vram = 0;
};

enum class LayoutHintUsage : uint8_t {
    IO = 0,
    PRIMARY = 1,
};

enum class LayoutHintFailureReason : uint8_t {
    NONE = 0,
    DATA_NULL,
    HINTS_EMPTY,
    SPLAT_COUNT_ZERO,
    HINT_COUNT_ZERO,
    HINT_START_OUT_OF_RANGE,
    HINT_RANGE_OUT_OF_RANGE,
    HINT_CHUNK_COUNT_OVERFLOW,
    HINT_NON_CONTIGUOUS_COVERAGE,
    HINT_OVERLAPPING_RANGES,
    REMAP_FLAG_REQUIRED,
    REMAP_FLAG_UNEXPECTED,
    REMAP_HINT_CHUNK_TOO_LARGE,
    REMAP_OFFSET_OUT_OF_RANGE,
    REMAP_TOTAL_COUNT_MISMATCH,
    REMAP_SOURCE_COUNT_MISMATCH,
    REMAP_SOURCE_INDEX_OUT_OF_RANGE,
    REMAP_SOURCE_INDEX_DUPLICATE,
    COUNT,
};

enum class LayoutHintFailureCategory : uint8_t {
    INPUT = 0,
    NON_CONTIGUOUS,
    INDEX_RANGE,
    REMAP,
    OTHER,
    COUNT,
};

struct LayoutHintValidationFailure {
    LayoutHintFailureReason reason = LayoutHintFailureReason::NONE;
    int hint_index = -1;
    uint64_t detail_a = 0;
    uint64_t detail_b = 0;
};

struct LayoutHintFailureCounters {
    uint64_t fallback_total = 0;
    uint64_t fallback_io_total = 0;
    uint64_t fallback_primary_total = 0;
    uint64_t strict_failure_total = 0;
    uint64_t category_counts[static_cast<int>(LayoutHintFailureCategory::COUNT)] = {};
    uint64_t reason_counts[static_cast<int>(LayoutHintFailureReason::COUNT)] = {};
    LayoutHintValidationFailure last_failure;
    LayoutHintUsage last_usage = LayoutHintUsage::IO;
    String last_context = "none";
};

struct LayoutHintFailureState {
    LayoutHintFailureCounters counters;
    LayoutHintValidationFailure last_io_failure;
    LayoutHintValidationFailure last_primary_failure;
    bool has_last_io_failure = false;
    bool has_last_primary_failure = false;
};

struct LayoutHintIndexRange {
    uint32_t start = 0;
    uint32_t end_exclusive = 0;
    int hint_index = -1;
};

HashMap<uint64_t, LayoutHintFailureState> g_layout_hint_failure_states;

uint64_t _layout_hint_state_key(const GaussianStreamingSystem *p_system) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p_system));
}

LayoutHintFailureState &_layout_hint_get_or_create_state(const GaussianStreamingSystem *p_system) {
    const uint64_t key = _layout_hint_state_key(p_system);
    LayoutHintFailureState *state = g_layout_hint_failure_states.getptr(key);
    if (!state) {
        g_layout_hint_failure_states.insert(key, LayoutHintFailureState());
        state = g_layout_hint_failure_states.getptr(key);
    }
    return *state;
}

const LayoutHintFailureState *_layout_hint_get_state(const GaussianStreamingSystem *p_system) {
    return g_layout_hint_failure_states.getptr(_layout_hint_state_key(p_system));
}

void _layout_hint_reset_state(const GaussianStreamingSystem *p_system) {
    g_layout_hint_failure_states.erase(_layout_hint_state_key(p_system));
}

bool _layout_hint_strict_validation_enabled() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return false;
    }
    if (gs::settings::get_bool(ps, "rendering/gaussian_splatting/streaming/layout_hint_validation_strict", false)) {
        return true;
    }
    return gs::settings::get_bool(ps, "rendering/gaussian_splatting/debug/layout_hint_validation_strict", false);
}

const char *_layout_hint_usage_code(LayoutHintUsage p_usage) {
    return p_usage == LayoutHintUsage::PRIMARY ? "primary" : "io";
}

const char *_layout_hint_reason_code(LayoutHintFailureReason p_reason) {
    switch (p_reason) {
        case LayoutHintFailureReason::NONE:
            return "none";
        case LayoutHintFailureReason::DATA_NULL:
            return "data_null";
        case LayoutHintFailureReason::HINTS_EMPTY:
            return "hints_empty";
        case LayoutHintFailureReason::SPLAT_COUNT_ZERO:
            return "splat_count_zero";
        case LayoutHintFailureReason::HINT_COUNT_ZERO:
            return "hint_count_zero";
        case LayoutHintFailureReason::HINT_START_OUT_OF_RANGE:
            return "hint_start_out_of_range";
        case LayoutHintFailureReason::HINT_RANGE_OUT_OF_RANGE:
            return "hint_range_out_of_range";
        case LayoutHintFailureReason::HINT_CHUNK_COUNT_OVERFLOW:
            return "hint_chunk_count_overflow";
        case LayoutHintFailureReason::HINT_NON_CONTIGUOUS_COVERAGE:
            return "hint_non_contiguous_coverage";
        case LayoutHintFailureReason::HINT_OVERLAPPING_RANGES:
            return "hint_overlapping_ranges";
        case LayoutHintFailureReason::REMAP_FLAG_REQUIRED:
            return "remap_flag_required";
        case LayoutHintFailureReason::REMAP_FLAG_UNEXPECTED:
            return "remap_flag_unexpected";
        case LayoutHintFailureReason::REMAP_HINT_CHUNK_TOO_LARGE:
            return "remap_hint_chunk_too_large";
        case LayoutHintFailureReason::REMAP_OFFSET_OUT_OF_RANGE:
            return "remap_offset_out_of_range";
        case LayoutHintFailureReason::REMAP_TOTAL_COUNT_MISMATCH:
            return "remap_total_count_mismatch";
        case LayoutHintFailureReason::REMAP_SOURCE_COUNT_MISMATCH:
            return "remap_source_count_mismatch";
        case LayoutHintFailureReason::REMAP_SOURCE_INDEX_OUT_OF_RANGE:
            return "remap_source_index_out_of_range";
        case LayoutHintFailureReason::REMAP_SOURCE_INDEX_DUPLICATE:
            return "remap_source_index_duplicate";
        case LayoutHintFailureReason::COUNT:
            break;
    }
    return "unknown";
}

LayoutHintFailureCategory _layout_hint_reason_category(LayoutHintFailureReason p_reason) {
    switch (p_reason) {
        case LayoutHintFailureReason::DATA_NULL:
        case LayoutHintFailureReason::HINTS_EMPTY:
        case LayoutHintFailureReason::SPLAT_COUNT_ZERO:
        case LayoutHintFailureReason::HINT_COUNT_ZERO:
            return LayoutHintFailureCategory::INPUT;
        case LayoutHintFailureReason::HINT_NON_CONTIGUOUS_COVERAGE:
        case LayoutHintFailureReason::HINT_OVERLAPPING_RANGES:
            return LayoutHintFailureCategory::NON_CONTIGUOUS;
        case LayoutHintFailureReason::HINT_START_OUT_OF_RANGE:
        case LayoutHintFailureReason::HINT_RANGE_OUT_OF_RANGE:
        case LayoutHintFailureReason::HINT_CHUNK_COUNT_OVERFLOW:
        case LayoutHintFailureReason::REMAP_HINT_CHUNK_TOO_LARGE:
        case LayoutHintFailureReason::REMAP_OFFSET_OUT_OF_RANGE:
        case LayoutHintFailureReason::REMAP_SOURCE_INDEX_OUT_OF_RANGE:
            return LayoutHintFailureCategory::INDEX_RANGE;
        case LayoutHintFailureReason::REMAP_FLAG_REQUIRED:
        case LayoutHintFailureReason::REMAP_FLAG_UNEXPECTED:
        case LayoutHintFailureReason::REMAP_TOTAL_COUNT_MISMATCH:
        case LayoutHintFailureReason::REMAP_SOURCE_COUNT_MISMATCH:
        case LayoutHintFailureReason::REMAP_SOURCE_INDEX_DUPLICATE:
            return LayoutHintFailureCategory::REMAP;
        case LayoutHintFailureReason::NONE:
        case LayoutHintFailureReason::COUNT:
            break;
    }
    return LayoutHintFailureCategory::OTHER;
}

const char *_layout_hint_category_code(LayoutHintFailureCategory p_category) {
    switch (p_category) {
        case LayoutHintFailureCategory::INPUT:
            return "input";
        case LayoutHintFailureCategory::NON_CONTIGUOUS:
            return "non_contiguous";
        case LayoutHintFailureCategory::INDEX_RANGE:
            return "index_range";
        case LayoutHintFailureCategory::REMAP:
            return "remap";
        case LayoutHintFailureCategory::OTHER:
        case LayoutHintFailureCategory::COUNT:
            return "other";
    }
    return "other";
}

String _layout_hint_failure_detail(const LayoutHintValidationFailure &p_failure) {
    if (p_failure.reason == LayoutHintFailureReason::NONE) {
        return "";
    }
    return vformat("hint_idx=%d detail_a=%d detail_b=%d",
            p_failure.hint_index,
            static_cast<int64_t>(p_failure.detail_a),
            static_cast<int64_t>(p_failure.detail_b));
}

void _layout_hint_set_last_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage,
        const LayoutHintValidationFailure &p_failure) {
    LayoutHintFailureState &state = _layout_hint_get_or_create_state(p_system);
    if (p_usage == LayoutHintUsage::PRIMARY) {
        state.last_primary_failure = p_failure;
        state.has_last_primary_failure = true;
    } else {
        state.last_io_failure = p_failure;
        state.has_last_io_failure = true;
    }
}

void _layout_hint_clear_last_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage) {
    LayoutHintFailureState &state = _layout_hint_get_or_create_state(p_system);
    if (p_usage == LayoutHintUsage::PRIMARY) {
        state.last_primary_failure = LayoutHintValidationFailure();
        state.has_last_primary_failure = false;
    } else {
        state.last_io_failure = LayoutHintValidationFailure();
        state.has_last_io_failure = false;
    }
}

LayoutHintValidationFailure _layout_hint_get_last_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage) {
    const LayoutHintFailureState *state = _layout_hint_get_state(p_system);
    if (!state) {
        return LayoutHintValidationFailure();
    }
    if (p_usage == LayoutHintUsage::PRIMARY) {
        return state->has_last_primary_failure ? state->last_primary_failure : LayoutHintValidationFailure();
    }
    return state->has_last_io_failure ? state->last_io_failure : LayoutHintValidationFailure();
}

String _layout_hint_record_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage,
        const String &p_context, bool p_strict_mode) {
    LayoutHintFailureState &state = _layout_hint_get_or_create_state(p_system);
    LayoutHintValidationFailure failure = _layout_hint_get_last_failure(p_system, p_usage);
    if (failure.reason == LayoutHintFailureReason::NONE) {
        failure.reason = LayoutHintFailureReason::HINTS_EMPTY;
    }

	LayoutHintFailureCounters &counters = state.counters;
	if (!p_strict_mode) {
		counters.fallback_total++;
		if (p_usage == LayoutHintUsage::PRIMARY) {
			counters.fallback_primary_total++;
		} else {
			counters.fallback_io_total++;
		}
	}
	if (p_strict_mode) {
		counters.strict_failure_total++;
	}

    const int reason_idx = static_cast<int>(failure.reason);
    if (reason_idx >= 0 && reason_idx < static_cast<int>(LayoutHintFailureReason::COUNT)) {
        counters.reason_counts[reason_idx]++;
    }
    const LayoutHintFailureCategory category = _layout_hint_reason_category(failure.reason);
    const int category_idx = static_cast<int>(category);
    if (category_idx >= 0 && category_idx < static_cast<int>(LayoutHintFailureCategory::COUNT)) {
        counters.category_counts[category_idx]++;
    }

    counters.last_failure = failure;
    counters.last_usage = p_usage;
    counters.last_context = p_context;

    const uint64_t reason_count = reason_idx >= 0 && reason_idx < static_cast<int>(LayoutHintFailureReason::COUNT)
            ? counters.reason_counts[reason_idx]
            : 0;
    const String detail = _layout_hint_failure_detail(failure);
    return vformat("[Streaming] %s layout hints rejected (usage=%s reason=%s category=%s fallback_count=%d reason_count=%d%s%s)",
            p_context,
            _layout_hint_usage_code(p_usage),
            _layout_hint_reason_code(failure.reason),
            _layout_hint_category_code(category),
            static_cast<int64_t>(counters.fallback_total),
            static_cast<int64_t>(reason_count),
            detail.is_empty() ? "" : " ",
            detail);
}

Dictionary _layout_hint_build_snapshot(const GaussianStreamingSystem *p_system, bool p_strict_mode_enabled) {
    Dictionary snapshot;
    snapshot["strict_mode_enabled"] = p_strict_mode_enabled;
    snapshot["fallback_total"] = static_cast<int64_t>(0);
    snapshot["fallback_io_total"] = static_cast<int64_t>(0);
    snapshot["fallback_primary_total"] = static_cast<int64_t>(0);
    snapshot["strict_failure_total"] = static_cast<int64_t>(0);
    snapshot["last_reason"] = String("none");
    snapshot["last_reason_category"] = String("other");
    snapshot["last_usage"] = String("none");
    snapshot["last_context"] = String("none");

    Dictionary category_counts;
    category_counts["input"] = static_cast<int64_t>(0);
    category_counts["non_contiguous"] = static_cast<int64_t>(0);
    category_counts["index_range"] = static_cast<int64_t>(0);
    category_counts["remap"] = static_cast<int64_t>(0);
    category_counts["other"] = static_cast<int64_t>(0);

    Dictionary reason_counts;
    for (int i = 1; i < static_cast<int>(LayoutHintFailureReason::COUNT); i++) {
        const LayoutHintFailureReason reason = static_cast<LayoutHintFailureReason>(i);
        reason_counts[_layout_hint_reason_code(reason)] = static_cast<int64_t>(0);
    }

    const LayoutHintFailureState *state = _layout_hint_get_state(p_system);
    if (!state) {
        snapshot["category_counts"] = category_counts;
        snapshot["reason_counts"] = reason_counts;
        return snapshot;
    }

    const LayoutHintFailureCounters &counters = state->counters;
    snapshot["fallback_total"] = static_cast<int64_t>(counters.fallback_total);
    snapshot["fallback_io_total"] = static_cast<int64_t>(counters.fallback_io_total);
    snapshot["fallback_primary_total"] = static_cast<int64_t>(counters.fallback_primary_total);
    snapshot["strict_failure_total"] = static_cast<int64_t>(counters.strict_failure_total);
    snapshot["last_reason"] = _layout_hint_reason_code(counters.last_failure.reason);
    snapshot["last_reason_category"] = _layout_hint_category_code(_layout_hint_reason_category(counters.last_failure.reason));
    snapshot["last_usage"] = _layout_hint_usage_code(counters.last_usage);
    snapshot["last_context"] = counters.last_context;

    for (int i = 0; i < static_cast<int>(LayoutHintFailureCategory::COUNT); i++) {
        const LayoutHintFailureCategory category = static_cast<LayoutHintFailureCategory>(i);
        category_counts[_layout_hint_category_code(category)] = static_cast<int64_t>(counters.category_counts[i]);
    }
    for (int i = 1; i < static_cast<int>(LayoutHintFailureReason::COUNT); i++) {
        const LayoutHintFailureReason reason = static_cast<LayoutHintFailureReason>(i);
        reason_counts[_layout_hint_reason_code(reason)] = static_cast<int64_t>(counters.reason_counts[i]);
    }

    snapshot["category_counts"] = category_counts;
    snapshot["reason_counts"] = reason_counts;
    return snapshot;
}

bool _validate_layout_hint_ranges(const Vector<GaussianStreamingSystem::ChunkLayoutHint> &p_hints,
        uint32_t p_index_space_count, bool p_use_source_offsets, bool p_require_remapped,
        bool p_forbid_remapped, bool p_allow_oversized_hints, LayoutHintValidationFailure &r_failure,
        uint64_t &r_total_hint_count, uint64_t &r_required_chunk_count, bool &r_saw_oversized_hint) {
    r_total_hint_count = 0;
    r_required_chunk_count = 0;
    r_saw_oversized_hint = false;
    if (p_hints.is_empty()) {
        r_failure.reason = LayoutHintFailureReason::HINTS_EMPTY;
        return false;
    }
    if (p_index_space_count == 0) {
        r_failure.reason = LayoutHintFailureReason::SPLAT_COUNT_ZERO;
        return false;
    }

    LocalVector<LayoutHintIndexRange> ranges;
    ranges.resize(p_hints.size());
    for (int i = 0; i < p_hints.size(); i++) {
        const GaussianStreamingSystem::ChunkLayoutHint &hint = p_hints[i];
        if (hint.count == 0) {
            r_failure.reason = LayoutHintFailureReason::HINT_COUNT_ZERO;
            r_failure.hint_index = i;
            return false;
        }
        if (p_require_remapped && !hint.source_indices_remapped) {
            r_failure.reason = LayoutHintFailureReason::REMAP_FLAG_REQUIRED;
            r_failure.hint_index = i;
            return false;
        }
        if (p_forbid_remapped && hint.source_indices_remapped) {
            r_failure.reason = LayoutHintFailureReason::REMAP_FLAG_UNEXPECTED;
            r_failure.hint_index = i;
            return false;
        }
        if (!p_allow_oversized_hints && hint.count > GaussianStreamingSystem::CHUNK_SIZE) {
            r_failure.reason = LayoutHintFailureReason::REMAP_HINT_CHUNK_TOO_LARGE;
            r_failure.hint_index = i;
            r_failure.detail_a = hint.count;
            r_failure.detail_b = GaussianStreamingSystem::CHUNK_SIZE;
            return false;
        }

        const uint32_t start = p_use_source_offsets ? hint.source_index_offset : hint.start_idx;
        if (start >= p_index_space_count) {
            r_failure.reason = LayoutHintFailureReason::HINT_START_OUT_OF_RANGE;
            r_failure.hint_index = i;
            r_failure.detail_a = start;
            r_failure.detail_b = p_index_space_count;
            return false;
        }
        const uint64_t end_exclusive_u64 = uint64_t(start) + uint64_t(hint.count);
        if (end_exclusive_u64 > p_index_space_count) {
            r_failure.reason = LayoutHintFailureReason::HINT_RANGE_OUT_OF_RANGE;
            r_failure.hint_index = i;
            r_failure.detail_a = end_exclusive_u64;
            r_failure.detail_b = p_index_space_count;
            return false;
        }

        ranges[i].start = start;
        ranges[i].end_exclusive = static_cast<uint32_t>(end_exclusive_u64);
        ranges[i].hint_index = i;

        r_total_hint_count += hint.count;
        const uint64_t split_count =
                (uint64_t(hint.count) + uint64_t(GaussianStreamingSystem::CHUNK_SIZE) - 1u) / uint64_t(GaussianStreamingSystem::CHUNK_SIZE);
        r_required_chunk_count += split_count;
        if (r_required_chunk_count > uint64_t(UINT32_MAX)) {
            r_failure.reason = LayoutHintFailureReason::HINT_CHUNK_COUNT_OVERFLOW;
            r_failure.hint_index = i;
            r_failure.detail_a = r_required_chunk_count;
            r_failure.detail_b = UINT32_MAX;
            return false;
        }
        r_saw_oversized_hint = r_saw_oversized_hint || hint.count > GaussianStreamingSystem::CHUNK_SIZE;
    }

    LayoutHintIndexRange *ranges_ptr = ranges.ptr();
    std::sort(ranges_ptr, ranges_ptr + ranges.size(), [](const LayoutHintIndexRange &a, const LayoutHintIndexRange &b) {
        if (a.start == b.start) {
            return a.end_exclusive < b.end_exclusive;
        }
        return a.start < b.start;
    });
    uint32_t expected_start = 0;
    for (uint32_t i = 0; i < ranges.size(); i++) {
        const LayoutHintIndexRange &range = ranges_ptr[i];
        if (range.start < expected_start) {
            r_failure.reason = LayoutHintFailureReason::HINT_OVERLAPPING_RANGES;
            r_failure.hint_index = range.hint_index;
            r_failure.detail_a = range.start;
            r_failure.detail_b = expected_start;
            return false;
        }
        if (range.start > expected_start) {
            r_failure.reason = LayoutHintFailureReason::HINT_NON_CONTIGUOUS_COVERAGE;
            r_failure.hint_index = range.hint_index;
            r_failure.detail_a = range.start;
            r_failure.detail_b = expected_start;
            return false;
        }
        expected_start = range.end_exclusive;
    }
    if (expected_start != p_index_space_count) {
        r_failure.reason = LayoutHintFailureReason::HINT_NON_CONTIGUOUS_COVERAGE;
        r_failure.hint_index = p_hints.size() - 1;
        r_failure.detail_a = expected_start;
        r_failure.detail_b = p_index_space_count;
        return false;
    }

    return true;
}

uint32_t _compute_async_enqueue_headroom(
        uint32_t p_queued_chunk_loads_this_frame,
        uint32_t p_max_chunk_loads_per_frame,
        uint32_t p_pack_jobs_in_flight,
        uint32_t p_max_pack_jobs_in_flight) {
    uint32_t frame_enqueue_headroom = UINT32_MAX;
    if (p_max_chunk_loads_per_frame > 0) {
        frame_enqueue_headroom = p_queued_chunk_loads_this_frame >= p_max_chunk_loads_per_frame
                ? 0
                : (p_max_chunk_loads_per_frame - p_queued_chunk_loads_this_frame);
    }

    uint32_t pack_enqueue_headroom = UINT32_MAX;
    if (p_max_pack_jobs_in_flight > 0) {
        pack_enqueue_headroom = p_pack_jobs_in_flight >= p_max_pack_jobs_in_flight
                ? 0
                : (p_max_pack_jobs_in_flight - p_pack_jobs_in_flight);
    }

    return MIN(frame_enqueue_headroom, pack_enqueue_headroom);
}

StreamingQueuePressureController::PressureSummary _summarize_queue_pressure_checked(
        const StreamingQueuePressureController::PressureSample &p_sample,
        const char *p_context) {
    StreamingQueuePressureController::PressureSummary summary =
            StreamingQueuePressureController::summarize(p_sample);
    String summary_error;
    if (StreamingQueuePressureController::validate_summary_invariants(summary, p_sample, &summary_error)) {
        return summary;
    }

    WARN_PRINT(vformat("[Streaming] Queue pressure summary invariant violated (%s): %s",
            p_context ? String(p_context) : String("unknown"),
            summary_error));
    summary.active = false;
    summary.cap_active = false;
    summary.pack_source_active = false;
    summary.upload_source_active = false;
    summary.sync_source_active = false;
    summary.backlog_depth = MAX(p_sample.sync_fallback_queue_depth,
            MAX(p_sample.pack_queue_depth, p_sample.upload_queue_depth));
    summary.source = StreamingQueuePressureController::SOURCE_NONE;
    summary.reason = StreamingQueuePressureController::REASON_NONE;
    return summary;
}

void _validate_queue_pressure_latched_state(bool &r_active, String &r_source, String &r_reason, const char *p_context) {
    String latch_error;
    if (StreamingQueuePressureController::validate_latched_state_invariants(r_active, r_source, r_reason, &latch_error)) {
        return;
    }
    WARN_PRINT(vformat("[Streaming] Queue pressure latch invariant violated (%s): %s",
            p_context ? String(p_context) : String("unknown"),
            latch_error));
    StreamingQueuePressureController::reset_latched_state(r_active, r_source, r_reason);
}

void _release_chunk_slot_if_matches(GaussianAtlasAllocator &p_allocator, uint64_t p_chunk_key, uint32_t p_expected_slot) {
    uint32_t mapped_slot = UINT32_MAX;
    if (!p_allocator.get_slot(p_chunk_key, mapped_slot)) {
        return;
    }
    if (p_expected_slot != UINT32_MAX && mapped_slot != p_expected_slot) {
        return;
    }
    p_allocator.release_slot(p_chunk_key);
}

bool _chunk_slot_matches_allocator(const GaussianAtlasAllocator &p_allocator, uint64_t p_chunk_key, uint32_t p_expected_slot, uint32_t *r_mapped_slot = nullptr) {
    uint32_t mapped_slot = UINT32_MAX;
    const bool has_slot = p_allocator.get_slot(p_chunk_key, mapped_slot);
    if (r_mapped_slot) {
        *r_mapped_slot = mapped_slot;
    }
    return has_slot && mapped_slot == p_expected_slot;
}

bool _record_streaming_invariant(bool p_invalid_state,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics,
        uint64_t &r_counter,
        const char *p_context,
        const String &p_message) {
    if (!p_invalid_state) {
        return false;
    }
    r_counter++;
    r_diagnostics.last_invariant_context = p_context;
    r_diagnostics.last_invariant_message = p_message;
    WARN_PRINT(p_message);
    return true;
}

bool _validate_pending_upload_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics) {
    if (_record_streaming_invariant(p_chunk.is_loaded, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d cannot be both loaded and upload_pending.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }
    if (_record_streaming_invariant(p_chunk.gpu_resident, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d pending upload cannot be GPU-resident.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }
    if (_record_streaming_invariant(p_chunk.buffer_slot == UINT32_MAX, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d upload_pending requires a valid buffer_slot.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }

    uint32_t mapped_slot = UINT32_MAX;
    const bool slot_match = _chunk_slot_matches_allocator(p_allocator, p_chunk_key, p_chunk.buffer_slot, &mapped_slot);
    return _record_streaming_invariant(!slot_match, r_diagnostics,
            r_diagnostics.invariant_slot_ownership_violations,
            p_context,
            vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d upload_pending slot=%d not tracked by allocator (mapped=%d).",
                    p_context, p_asset_id, p_chunk_idx, p_chunk.buffer_slot,
                    mapped_slot == UINT32_MAX ? -1 : int(mapped_slot)));
}

bool _validate_loaded_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics) {
    if (_record_streaming_invariant(!p_chunk.gpu_resident, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d loaded chunk is not GPU-resident.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }
    if (_record_streaming_invariant(p_chunk.buffer_slot == UINT32_MAX, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d loaded chunk is missing buffer_slot.",
                        p_context, p_asset_id, p_chunk_idx))) {
        return true;
    }

    uint32_t mapped_slot = UINT32_MAX;
    const bool slot_match = _chunk_slot_matches_allocator(p_allocator, p_chunk_key, p_chunk.buffer_slot, &mapped_slot);
    return _record_streaming_invariant(!slot_match, r_diagnostics,
            r_diagnostics.invariant_slot_ownership_violations,
            p_context,
            vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d loaded slot=%d not tracked by allocator (mapped=%d).",
                    p_context, p_asset_id, p_chunk_idx, p_chunk.buffer_slot,
                    mapped_slot == UINT32_MAX ? -1 : int(mapped_slot)));
}

void _validate_idle_chunk_invariant(const GaussianAtlasAllocator &p_allocator,
        const GaussianStreamingTypes::StreamingChunk &p_chunk,
        uint64_t p_chunk_key,
        uint32_t p_asset_id,
        uint32_t p_chunk_idx,
        const char *p_context,
        bool p_allow_deferred_allocator_release,
        GaussianStreamingTypes::DiagnosticsState &r_diagnostics) {
    uint32_t mapped_slot = UINT32_MAX;
    const bool slot_tracked = p_allocator.get_slot(p_chunk_key, mapped_slot);
    if (_record_streaming_invariant(p_chunk.buffer_slot != UINT32_MAX, r_diagnostics,
                r_diagnostics.invariant_upload_lifecycle_violations,
                p_context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d has buffer_slot=%d while not loaded/pending.",
                        p_context, p_asset_id, p_chunk_idx, p_chunk.buffer_slot))) {
        return;
    }
    if (p_allow_deferred_allocator_release) {
        return;
    }
    _record_streaming_invariant(slot_tracked, r_diagnostics,
            r_diagnostics.invariant_slot_ownership_violations,
            p_context,
            vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d allocator tracks slot=%d while chunk is not loaded/pending.",
                    p_context, p_asset_id, p_chunk_idx, mapped_slot));
}

void _subtract_pending_upload_bytes(GaussianStreamingTypes::BudgetState &r_budget, uint64_t p_bytes) {
    r_budget.pending_upload_bytes = r_budget.pending_upload_bytes > p_bytes
            ? (r_budget.pending_upload_bytes - p_bytes)
            : 0;
}

void _release_pending_upload_slot(GaussianStreamingTypes::BudgetState &r_budget) {
    if (r_budget.pending_upload_slots > 0) {
        r_budget.pending_upload_slots--;
    }
}

template <typename UploadRetirementTicket>
void _release_failed_upload_retirement(GaussianAtlasAllocator &r_allocator,
        GaussianStreamingTypes::BudgetState &r_budget,
        uint64_t &r_last_completed_upload_ticket_id,
        const UploadRetirementTicket &p_ticket,
        uint64_t p_chunk_key) {
    _release_chunk_slot_if_matches(r_allocator, p_chunk_key, p_ticket.buffer_slot);
    _subtract_pending_upload_bytes(r_budget, p_ticket.bytes);
    _release_pending_upload_slot(r_budget);
    r_budget.failed_upload_retirements++;
    r_last_completed_upload_ticket_id = p_ticket.ticket_id;
}

template <typename UploadRetirementTicket>
void _release_cancelled_upload_retirement(GaussianAtlasAllocator &r_allocator,
        GaussianStreamingTypes::BudgetState &r_budget,
        uint64_t &r_last_completed_upload_ticket_id,
        const UploadRetirementTicket &p_ticket,
        uint64_t p_chunk_key) {
    _release_chunk_slot_if_matches(r_allocator, p_chunk_key, p_ticket.buffer_slot);
    _subtract_pending_upload_bytes(r_budget, p_ticket.bytes);
    _release_pending_upload_slot(r_budget);
    r_last_completed_upload_ticket_id = p_ticket.ticket_id;
}

template <typename UploadRetirementTicket>
bool _retirement_ticket_matches_chunk(const UploadRetirementTicket &p_ticket,
        const GaussianStreamingTypes::StreamingChunk &p_chunk) {
    return p_chunk.upload_pending && !p_chunk.is_loaded &&
            p_chunk.buffer_slot == p_ticket.buffer_slot &&
            p_chunk.upload_ticket_id == p_ticket.ticket_id;
}

template <typename UploadRetirementTicket>
bool _retirement_state_mismatch_can_rollback(const UploadRetirementTicket &p_ticket,
        const GaussianStreamingTypes::StreamingChunk &p_chunk) {
    return !p_chunk.is_loaded && p_chunk.buffer_slot == p_ticket.buffer_slot;
}

template <typename UploadRetirementTicket>
void _mark_upload_ticket_gpu_retired(GaussianStreamingTypes::StreamingChunk &r_chunk,
        const UploadRetirementTicket &p_ticket) {
    r_chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_GPU_RETIRED;
    r_chunk.upload_completion_mode = p_ticket.completion_mode;
}

template <typename UploadRetirementTicket>
void _record_successful_upload_retirement(GaussianStreamingTypes::BudgetState &r_budget,
        SHCompressionMetrics &r_total_metrics,
        uint64_t &r_last_completed_upload_ticket_id,
        String &r_last_upload_completion_mode,
        const UploadRetirementTicket &p_ticket) {
    r_budget.chunks_loaded_this_frame++;
    r_budget.retired_upload_bytes_this_frame += p_ticket.bytes;
    r_budget.retired_upload_slots_this_frame++;
    r_total_metrics.raw_bytes += p_ticket.metrics.raw_bytes;
    r_total_metrics.compressed_bytes += p_ticket.metrics.compressed_bytes;
    r_total_metrics.coefficient_count += p_ticket.metrics.coefficient_count;
    r_last_completed_upload_ticket_id = p_ticket.ticket_id;
    r_last_upload_completion_mode = _streaming_upload_completion_mode_name(p_ticket.completion_mode);
}

bool _is_finite_transform3d(const Transform3D &p_transform) {
    if (!Math::is_finite(p_transform.origin.x) ||
            !Math::is_finite(p_transform.origin.y) ||
            !Math::is_finite(p_transform.origin.z)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!Math::is_finite(p_transform.basis[i][0]) ||
                !Math::is_finite(p_transform.basis[i][1]) ||
                !Math::is_finite(p_transform.basis[i][2])) {
            return false;
        }
    }
    return true;
}

bool _is_finite_projection(const Projection &p_projection) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            if (!Math::is_finite(p_projection.columns[col][row])) {
                return false;
            }
        }
    }
    return true;
}

uint32_t _expand_morton_bits_10(uint32_t p_value) {
    uint32_t v = p_value & 1023u;
    v = (v | (v << 16)) & 0x030000FFu;
    v = (v | (v << 8)) & 0x0300F00Fu;
    v = (v | (v << 4)) & 0x030C30C3u;
    v = (v | (v << 2)) & 0x09249249u;
    return v;
}

uint32_t _compute_morton_code_10(const Vector3 &p_position, const AABB &p_bounds) {
    const Vector3 bounds_min = p_bounds.position;
    const Vector3 bounds_size = p_bounds.size;
    const float norm_x = bounds_size.x > 1e-6f ? (p_position.x - bounds_min.x) / bounds_size.x : 0.5f;
    const float norm_y = bounds_size.y > 1e-6f ? (p_position.y - bounds_min.y) / bounds_size.y : 0.5f;
    const float norm_z = bounds_size.z > 1e-6f ? (p_position.z - bounds_min.z) / bounds_size.z : 0.5f;

    const float clamped_x = CLAMP(Math::is_finite(norm_x) ? norm_x : 0.5f, 0.0f, 1.0f);
    const float clamped_y = CLAMP(Math::is_finite(norm_y) ? norm_y : 0.5f, 0.0f, 1.0f);
    const float clamped_z = CLAMP(Math::is_finite(norm_z) ? norm_z : 0.5f, 0.0f, 1.0f);

    const uint32_t qx = uint32_t(clamped_x * 1023.0f);
    const uint32_t qy = uint32_t(clamped_y * 1023.0f);
    const uint32_t qz = uint32_t(clamped_z * 1023.0f);
    return (_expand_morton_bits_10(qz) << 2) | (_expand_morton_bits_10(qy) << 1) | _expand_morton_bits_10(qx);
}

} // namespace

// ============================================================================
// PackTelemetry implementation (DEV_ENABLED only)
// ============================================================================

GaussianStreamingSystem::GaussianStreamingSystem() {
    // Initialize frame data
    for (int i = 0; i < RING_BUFFER_FRAMES; i++) {
        frame_data[i].frame_number = 0;
    }
    _layout_hint_reset_state(this);
    _connect_project_settings();
}

void GaussianStreamingSystem::_connect_project_settings() {
    if (project_settings_connected) {
        return;
    }
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }
    Callable callback = callable_mp(this, &GaussianStreamingSystem::_on_project_settings_changed);
    if (!ps->is_connected("settings_changed", callback)) {
        ps->connect("settings_changed", callback);
    }
    project_settings_connected = true;
}

void GaussianStreamingSystem::_on_project_settings_changed() {
    config_dirty = true;
}

GaussianStreamingSystem::~GaussianStreamingSystem() {
    _layout_hint_reset_state(this);
    _stop_pack_threads();
    _clear_pending_uploads();

    // Clean up GPU resources
    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    RenderingDevice *rd = primary_device_override ? primary_device_override
            : (last_upload_device ? last_upload_device
                    : (manager ? manager->get_primary_rendering_device() : nullptr));
    _release_persistent_buffer(rd, "destructor");
    global_atlas_registry.cleanup(rd);

    // Clean up quantization buffer
    _release_quantization_buffer(rd, "destructor", false);
}

void GaussianStreamingSystem::_release_persistent_buffer(RenderingDevice *p_rd, const char *p_context) {
    if (!persistent_buffer.is_valid()) {
        if (persistent_buffer_size != 0) {
            WARN_PRINT(vformat("[Streaming] %s: persistent buffer size (%d) was non-zero with invalid RID; resetting state.",
                    p_context ? p_context : "persistent_buffer_release",
                    persistent_buffer_size));
        }
        persistent_buffer_size = 0;
        return;
    }

    if (p_rd) {
        p_rd->free(persistent_buffer);
    } else {
        WARN_PRINT(vformat("[Streaming] %s: persistent buffer RID %s exists but RenderingDevice is null; dropping stale RID handle.",
                p_context ? p_context : "persistent_buffer_release",
                String::num_uint64(static_cast<uint64_t>(persistent_buffer.get_id()))));
    }

    persistent_buffer = RID();
    persistent_buffer_size = 0;
}

bool GaussianStreamingSystem::_grow_persistent_buffer(uint32_t p_new_capacity) {
    if (!streaming_initialized) {
        return false;
    }
    if (streaming_current_capacity == 0 || streaming_max_capacity == 0) {
        return false;
    }
    if (streaming_current_capacity >= streaming_max_capacity) {
        return false;
    }

    // Clamp to [current+1, streaming_max_capacity].
    const uint32_t min_target = streaming_current_capacity + 1u;
    uint32_t new_capacity = MAX(p_new_capacity, min_target);
    new_capacity = MIN(new_capacity, streaming_max_capacity);
    if (new_capacity <= streaming_current_capacity) {
        return false;
    }

    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    RenderingDevice *rd = primary_device_override ? primary_device_override
            : (last_upload_device ? last_upload_device
                    : (manager ? manager->get_primary_rendering_device() : nullptr));
    if (!rd) {
        ERR_PRINT("[Streaming] _grow_persistent_buffer failed: no RenderingDevice available.");
        return false;
    }

    const uint64_t slot_bytes = _streaming_chunk_slot_bytes();
    const uint64_t new_bytes64 = uint64_t(new_capacity) * slot_bytes;
    if (slot_bytes == 0 || new_bytes64 == 0 || new_bytes64 > uint64_t(UINT32_MAX)) {
        ERR_PRINT(vformat("[Streaming] _grow_persistent_buffer failed: new buffer size overflow (%s bytes, max=%u).",
                String::num_uint64(new_bytes64), UINT32_MAX));
        return false;
    }

    const uint32_t new_buffer_size = static_cast<uint32_t>(new_bytes64);
    RID new_buffer = rd->storage_buffer_create(new_buffer_size);
    if (!new_buffer.is_valid()) {
        ERR_PRINT(vformat("[Streaming] _grow_persistent_buffer failed: storage_buffer_create returned invalid RID (size=%u bytes).",
                new_buffer_size));
        return false;
    }
    rd->set_resource_name(new_buffer, "GS_Streaming_PersistentBuffer");

    // Copy currently resident slot contents from old buffer to new buffer. The
    // slot layout (slot index -> byte offset) is identical for old and new
    // buffers (only the trailing capacity differs), so this is a flat copy of
    // the old buffer bytes; trailing slots stay zero-initialized.
    const uint32_t copy_size = persistent_buffer_size;
    if (copy_size > 0 && persistent_buffer.is_valid()) {
        if (copy_size > new_buffer_size) {
            ERR_PRINT(vformat("[Streaming] _grow_persistent_buffer failed: invariant violation old=%u > new=%u.",
                    copy_size, new_buffer_size));
            rd->free(new_buffer);
            return false;
        }
        const Error copy_err = rd->buffer_copy(persistent_buffer, new_buffer, 0, 0, copy_size);
        if (copy_err != OK) {
            ERR_PRINT(vformat("[Streaming] _grow_persistent_buffer failed: buffer_copy err=%d.", int(copy_err)));
            rd->free(new_buffer);
            return false;
        }
        gs_device_utils::safe_submit_and_sync(rd);
    }

    // Preserve allocator slot assignments before swapping RIDs so any caller
    // reading state mid-rollback sees a consistent view.
    if (!atlas_allocator.resize_preserve(new_capacity)) {
        ERR_PRINT(vformat("[Streaming] _grow_persistent_buffer failed: atlas_allocator.resize_preserve refused %u (current=%u).",
                new_capacity, streaming_current_capacity));
        rd->free(new_buffer);
        return false;
    }

    // Swap buffers; release the old one via the existing pattern.
    const uint32_t prev_capacity = streaming_current_capacity;
    _release_persistent_buffer(rd, "_grow_persistent_buffer");
    persistent_buffer = new_buffer;
    persistent_buffer_size = new_buffer_size;
    streaming_current_capacity = new_capacity;
    streaming_grow_count++;

    // Force a global atlas resync at end-of-frame so downstream descriptor
    // caches (which key on atlas_generation / asset_registry_dirty) rebind to
    // the new buffer RID before the next frame's binding. The existing
    // end-of-update sync_to_gpu() picks this up — calling sync_to_gpu()
    // directly here would re-enter atlas bookkeeping mid-frame.
    global_atlas_registry.mark_asset_registry_dirty();

    GS_LOG_STREAMING_INFO(vformat("[Streaming] Persistent buffer grew %d -> %d slots (max=%d, grow_count=%d).",
            prev_capacity, streaming_current_capacity,
            streaming_max_capacity, streaming_grow_count));
    return true;
}

bool GaussianStreamingSystem::_try_grow_persistent_buffer_for_atlas_pressure(uint32_t p_loaded_chunks,
        uint32_t p_effective_max,
        bool p_enforce_vram_regulator_gate,
        bool p_vram_regulator_allows_load) {
    if (atlas_allocator.has_free_slots()) {
        return false;
    }
    if (streaming_current_capacity == 0 || streaming_current_capacity >= streaming_max_capacity) {
        return false;
    }
    if (p_effective_max == 0 || p_loaded_chunks >= p_effective_max) {
        return false;
    }
    if (p_enforce_vram_regulator_gate && !p_vram_regulator_allows_load) {
        return false;
    }

    const uint32_t growth_ceiling = MIN(streaming_max_capacity, p_effective_max);
    if (streaming_current_capacity >= growth_ceiling) {
        return false;
    }

    uint64_t target64 = MIN<uint64_t>(uint64_t(streaming_current_capacity) * 2u,
            uint64_t(growth_ceiling));

    // Allocation-inclusive growth gate. The regulator is fed the *evictable* usage so
    // eviction/admission-into-existing-slots ignore the non-reclaimable persistent buffer
    // (see _update_vram_regulator). Growth is different: it ENLARGES that allocation, so
    // p_vram_regulator_allows_load (derived from can_load_more_chunks on reclaimable usage)
    // would stay under threshold and let the buffer grow past budget while get_vram_usage()
    // is already over. Bound the growth target by the budget instead, so we never grow the
    // persistent buffer beyond the configured VRAM budget. CLAMP the target down to the
    // largest in-budget slot count rather than refusing growth outright: with room for some
    // (but not the doubled) slots we should still grow as far as the budget allows, or a
    // low-budget config would plateau the atlas below its usable capacity. (Codex #411)
    if (p_enforce_vram_regulator_gate && budget.vram_regulator.is_valid()) {
        const uint64_t budget_bytes = budget.vram_regulator->get_debug_stats().budget_bytes;
        if (budget_bytes > 0) {
            const uint64_t chunk_bytes = uint64_t(CHUNK_SIZE) * sizeof(PackedGaussian);
            const uint64_t aux_bytes = _get_auxiliary_vram_overhead_bytes();
            // Largest persistent-buffer slot count whose allocation-inclusive total
            // (slots*chunk_bytes + aux; the grown buffer dominates the payload) fits the budget.
            const uint64_t budget_for_persistent = budget_bytes > aux_bytes ? (budget_bytes - aux_bytes) : 0;
            const uint64_t budget_max_slots = chunk_bytes > 0 ? budget_for_persistent / chunk_bytes : 0;
            if (budget_max_slots <= uint64_t(streaming_current_capacity)) {
                return false; // no in-budget headroom to grow into
            }
            target64 = MIN(target64, budget_max_slots);
        }
    }

    if (target64 <= uint64_t(streaming_current_capacity)) {
        return false;
    }
    return _grow_persistent_buffer(static_cast<uint32_t>(target64));
}

void GaussianStreamingSystem::_bind_methods() {
    ClassDB::bind_method(D_METHOD("initialize", "data"), &GaussianStreamingSystem::initialize);
    ClassDB::bind_method(D_METHOD("attach_memory_stream", "stream"), &GaussianStreamingSystem::attach_memory_stream);
    ClassDB::bind_method(D_METHOD("update_streaming", "camera_pos", "projection", "frame_delta_seconds"),
            &GaussianStreamingSystem::update_streaming, DEFVAL(-1.0f));
    ClassDB::bind_method(D_METHOD("begin_residency_requests"), &GaussianStreamingSystem::begin_residency_requests);
    ClassDB::bind_method(D_METHOD("request_chunk_residency", "asset_id", "chunk_id", "lod_level"),
            &GaussianStreamingSystem::request_chunk_residency);
    ClassDB::bind_method(D_METHOD("request_asset_residency", "asset_id", "lod_level"),
            &GaussianStreamingSystem::request_asset_residency);
    ClassDB::bind_method(D_METHOD("finalize_residency_requests"), &GaussianStreamingSystem::finalize_residency_requests);
    ClassDB::bind_method(D_METHOD("get_residency_request_status", "asset_id", "chunk_id"),
            &GaussianStreamingSystem::get_residency_request_status);
    ClassDB::bind_method(D_METHOD("get_visible_count"), &GaussianStreamingSystem::get_visible_count);
    ClassDB::bind_method(D_METHOD("begin_frame"), &GaussianStreamingSystem::begin_frame);
    ClassDB::bind_method(D_METHOD("end_frame"), &GaussianStreamingSystem::end_frame);
    ClassDB::bind_method(D_METHOD("get_vram_usage"), &GaussianStreamingSystem::get_vram_usage);
    ClassDB::bind_method(D_METHOD("get_loaded_chunks"), &GaussianStreamingSystem::get_loaded_chunks);
    // LocalVector is an internal container and not exposed to the scripting API,
    // so we intentionally avoid binding get_visible_indices() here.
    ClassDB::bind_method(D_METHOD("get_task_debug_state"), &GaussianStreamingSystem::get_task_debug_state);
    ClassDB::bind_method(D_METHOD("get_streaming_analytics"), &GaussianStreamingSystem::get_streaming_analytics);

    // Chunk frustum culling configuration
    ClassDB::bind_method(D_METHOD("set_chunk_frustum_culling_enabled", "enabled"), &GaussianStreamingSystem::set_chunk_frustum_culling_enabled);
    ClassDB::bind_method(D_METHOD("is_chunk_frustum_culling_enabled"), &GaussianStreamingSystem::is_chunk_frustum_culling_enabled);
    ClassDB::bind_method(D_METHOD("set_chunk_frustum_padding", "padding"), &GaussianStreamingSystem::set_chunk_frustum_padding);
    ClassDB::bind_method(D_METHOD("get_chunk_frustum_padding"), &GaussianStreamingSystem::get_chunk_frustum_padding);
    ClassDB::bind_method(D_METHOD("get_chunk_culling_stats"), &GaussianStreamingSystem::get_chunk_culling_stats);

    // VRAM budget regulation
    ClassDB::bind_method(D_METHOD("get_vram_debug_stats"), &GaussianStreamingSystem::get_vram_debug_stats);
    ClassDB::bind_method(D_METHOD("is_vram_budget_warning_active"), &GaussianStreamingSystem::is_vram_budget_warning_active);
    ClassDB::bind_method(D_METHOD("get_effective_max_chunks"), &GaussianStreamingSystem::get_effective_max_chunks);
    ClassDB::bind_method(D_METHOD("get_pending_upload_retirement_slots"), &GaussianStreamingSystem::get_pending_upload_retirement_slots);
    ClassDB::bind_method(D_METHOD("get_pending_upload_retirement_bytes"), &GaussianStreamingSystem::get_pending_upload_retirement_bytes);

    // Distance-based LOD (Octree-GS)
    ClassDB::bind_method(D_METHOD("get_lod_debug_stats"), &GaussianStreamingSystem::get_lod_debug_stats);
    ClassDB::bind_method(D_METHOD("get_effective_splat_count"), &GaussianStreamingSystem::get_effective_splat_count);
}

void GaussianStreamingSystem::initialize(Ref<::GaussianData> p_data) {
    _connect_project_settings();
    _layout_hint_reset_state(this);
    streaming_initialized = false;
    last_streaming_update_usec = 0;
    last_streaming_frame_delta_seconds = ESTIMATED_FRAME_DELTA_60FPS;
    effective_max_guard_warning_emitted = false;
    effective_max_guard_warning_regulated = 0;
    effective_max_guard_warning_capacity = 0;
    runtime_capacity_guard_logged = false;
    runtime_capacity_guard_effective_max = UINT32_MAX;
    runtime_capacity_guard_runtime_capacity = UINT32_MAX;
    runtime_capacity_guard_buffer_valid = true;
    runtime_capacity_guard_initialized = true;
    // PR #352: clear the warned-once latch on every (re-)initialize so a
    // subsequent failure can re-arm the one-shot ERR. Successful init naturally
    // leaves the flag false; failed init below will set it to true.
    failed_init_warning_emitted = false;
    invalid_camera_input_events = 0;
    last_invalid_camera_log_frame = UINT64_MAX;
    visibility.reset_runtime_state();
    diagnostics = DiagnosticsState();
    budget.pending_upload_bytes = 0;
    budget.pending_upload_slots = 0;
    budget.retired_upload_bytes_this_frame = 0;
    budget.retired_upload_slots_this_frame = 0;
    budget.failed_upload_retirements = 0;
    pending_upload_retirements.clear();
    next_upload_ticket_id = 1;
    last_completed_upload_ticket_id = 0;
    last_upload_completion_mode = "none";
    analytics_snapshot.clear();
    scheduler.visible_scan_cursor = 0;
    scheduler.prefetch_scan_cursor = 0;
    scheduler.last_visible_scan_count = 0;
    scheduler.last_visible_scan_budget_effective = 0;
    scheduler.last_load_candidate_count = 0;
    scheduler.last_primary_eviction_scan_count = 0;
    scheduler.last_primary_eviction_candidate_count = 0;
    scheduler.last_non_primary_scan_count = 0;
    scheduler.last_non_primary_eviction_candidate_count = 0;
    scheduler.last_prefetch_scan_count = 0;
    scheduler.last_prefetch_scan_budget_effective = 0;
    scheduler.last_prefetch_candidate_count = 0;
    scheduler.last_prefetch_upload_pending_skip_count = 0;
    scheduler.last_prefetch_enqueued_count = 0;
    scheduler.last_prefetch_enqueue_headroom_stall_count = 0;
    scheduler.last_sync_fallback_queue_depth = 0;
    scheduler.last_sync_fallback_enqueued_count = 0;
    scheduler.last_sync_fallback_drained_count = 0;
    scheduler.last_sync_fallback_dropped_count = 0;
    scheduler.last_sync_fallback_stalled_count = 0;
    scheduler.queue_pressure_candidate_scan_throttle_active = false;
    scheduler.queue_pressure_candidate_scan_throttle_queue_depth = 0;
    scheduler.force_sync_fallback_due_to_async_stall = false;
    scheduler.sync_fallback_chunk_load_queue.clear();
    scheduler.sync_fallback_chunk_load_set.clear();
    scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
    scheduler.last_update_cpu_ms = 0.0;
    scheduler.last_visibility_cpu_ms = 0.0;
    scheduler.last_load_cpu_ms = 0.0;
    scheduler.last_build_visible_cpu_ms = 0.0;
    scheduler.last_prefetch_cpu_ms = 0.0;
    scheduler.last_sync_fallback_cpu_ms = 0.0;
    scheduler.last_cpu_total_attributed_ms = 0.0;
    scheduler.last_cpu_unattributed_ms = 0.0;
    source_data = p_data;
    if (source_data.is_null()) {
        per_chunk_quantization_dc_compatible = true;
        GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
        RenderingDevice *rd = primary_device_override ? primary_device_override
                : (last_upload_device ? last_upload_device
                        : (manager ? manager->get_primary_rendering_device() : nullptr));
        global_atlas_registry.cleanup(rd);
        atlas_allocator.clear();
        chunks.clear();
        eviction_controller.invalidate_resident_tracking();
        asset_registry.primary_chunk_source_indices.clear();
        primary_chunk_layout_metrics.reset();
        streaming_initial_capacity = 0;
        streaming_current_capacity = 0;
        streaming_max_capacity = 0;
        streaming_grow_count = 0;
        return;
    }

    total_splat_count = source_data->get_count();
    total_sh_metrics = SHCompressionMetrics();
    budget.evicted_bytes_total = 0;
    _load_quantization_config_from_project_settings();
    if (!_create_chunks()) {
        if (!failed_init_warning_emitted) {
            ERR_PRINT("[Streaming] Initialization failed: strict primary layout validation rejected chunk layout hints.");
            failed_init_warning_emitted = true;
        }
        streaming_initialized = false;
        return;
    }
    _register_primary_asset();
    _refresh_quantization_dc_compatibility();

    // Allocate persistent GPU buffer for maximum loaded chunks
    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    RenderingDevice *rd = primary_device_override ? primary_device_override
            : (last_upload_device ? last_upload_device
                    : (manager ? manager->get_primary_rendering_device() : nullptr));
    last_upload_device = rd;
    global_atlas_registry.cleanup(rd);
    _release_persistent_buffer(rd, "initialize");

    // Initialize VRAM budget regulator
    budget.vram_regulator.instantiate();
    budget.vram_regulator->initialize(rd);
    _apply_config_overrides();

    // Use regulated max chunks for buffer allocation.
    uint32_t effective_max_chunks = get_effective_max_chunks();
    const uint32_t addressable_max_chunks = _streaming_addressable_chunk_limit();
    if (addressable_max_chunks == 0) {
        if (!failed_init_warning_emitted) {
            ERR_PRINT("[Streaming] Initialization failed: chunk slot byte size is invalid for 32-bit RenderingDevice buffer addressing.");
            failed_init_warning_emitted = true;
        }
        streaming_initialized = false;
        return;
    }
    if (effective_max_chunks > addressable_max_chunks) {
        WARN_PRINT(vformat("[Streaming] Capping effective_max_chunks from %d to %d due to 32-bit buffer addressing limit.",
                effective_max_chunks, addressable_max_chunks));
        effective_max_chunks = addressable_max_chunks;
    }

    // Phase 3: size the persistent buffer to the actual asset chunk count plus
    // a growth headroom, instead of the regulated maximum. Keeps startup VRAM
    // proportional to the loaded scene; growth is wired into the eviction
    // pressure path via _grow_persistent_buffer().
    const uint32_t asset_chunks = chunks.size();
    const uint32_t headroom = MAX<uint32_t>(2u, asset_chunks / 4u);
    uint32_t initial_capacity = asset_chunks + headroom;
    const uint32_t min_floor = STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM;
    initial_capacity = MAX(initial_capacity, min_floor);
    initial_capacity = MIN(initial_capacity, effective_max_chunks);

    if (rd) {
        GS_STARTUP_SCOPE("streaming_persistent_buffer_alloc");
        const uint64_t persistent_bytes64 = uint64_t(initial_capacity) * _streaming_chunk_slot_bytes();
        if (persistent_bytes64 == 0 || persistent_bytes64 > uint64_t(UINT32_MAX)) {
            if (!failed_init_warning_emitted) {
                ERR_PRINT(vformat("[Streaming] Initialization failed: persistent buffer size overflow (%s bytes, max=%u).",
                        String::num_uint64(persistent_bytes64), UINT32_MAX));
                failed_init_warning_emitted = true;
            }
            persistent_buffer = RID();
            persistent_buffer_size = 0;
        } else {
            persistent_buffer_size = static_cast<uint32_t>(persistent_bytes64);
            persistent_buffer = rd->storage_buffer_create(persistent_buffer_size);
            rd->set_resource_name(persistent_buffer, "GS_Streaming_PersistentBuffer");
        }
    }

    // Initialize atlas allocator slots to the right-sized initial capacity.
    atlas_allocator.reset(initial_capacity);
    streaming_initial_capacity = initial_capacity;
    streaming_current_capacity = initial_capacity;
    streaming_max_capacity = effective_max_chunks;
    streaming_grow_count = 0;
    const uint32_t runtime_capacity_max = _compute_runtime_chunk_capacity_limit();
    const bool persistent_buffer_valid = persistent_buffer.is_valid() && persistent_buffer_size > 0;
    if (effective_max_chunks == 0 || runtime_capacity_max == 0 || !persistent_buffer_valid) {
        if (!failed_init_warning_emitted) {
            ERR_PRINT(vformat("[Streaming] Initialization failed: runtime not loadable (effective_max=%d, runtime_capacity=%d, persistent_buffer_valid=%s, device_present=%s).",
                    effective_max_chunks,
                    runtime_capacity_max,
                    persistent_buffer_valid ? "yes" : "no",
                    rd ? "yes" : "no"));
            failed_init_warning_emitted = true;
        }
        streaming_initialized = false;
        return;
    }
    streaming_initialized = true;

    _load_streaming_tuning_config_from_project_settings();
    _reload_debug_logging_config();

    if (is_per_chunk_quantization_enabled()) {
        // Upload quantization buffer to GPU
        if (rd) {
            _upload_quantization_buffer(rd);
        }

        GS_LOG_STREAMING_INFO(vformat("[Streaming] Per-chunk quantization enabled: %d bits position, %s",
                quantization_position_bits,
                quantization_scales_enabled ? vformat("%d bits scale", quantization_scale_bits) : "scale unquantized"));
    }

    {
        GS_STARTUP_SCOPE("streaming_atlas_build_cpu");
        global_atlas_registry.build_cpu_state(*this);
    }
    {
        GS_STARTUP_SCOPE("streaming_atlas_sync_gpu");
        global_atlas_registry.sync_to_gpu(*this, rd);
    }

    GS_LOG_STREAMING_INFO(vformat("[Streaming] Initialized with %d chunks for %d splats (VRAM budget: %d MB, initial slots: %d, max slots: %d)",
            chunks.size(), total_splat_count,
            budget.vram_regulator->get_config().budget_mb,
            streaming_initial_capacity, streaming_max_capacity));
}
void GaussianStreamingSystem::initialize_with_device(Ref<::GaussianData> p_data, RenderingDevice *p_device) {
    // Temporarily override the device used during initialization
    // This ensures persistent_buffer is created on the same device as the renderer
    primary_device_override = p_device;
    initialize(p_data);
    primary_device_override = nullptr;
}

void GaussianStreamingSystem::initialize_empty(RenderingDevice *p_device) {
    _connect_project_settings();
    _layout_hint_reset_state(this);
    streaming_initialized = false;
    last_streaming_update_usec = 0;
    last_streaming_frame_delta_seconds = ESTIMATED_FRAME_DELTA_60FPS;
    effective_max_guard_warning_emitted = false;
    effective_max_guard_warning_regulated = 0;
    effective_max_guard_warning_capacity = 0;
    runtime_capacity_guard_logged = false;
    runtime_capacity_guard_effective_max = UINT32_MAX;
    runtime_capacity_guard_runtime_capacity = UINT32_MAX;
    runtime_capacity_guard_buffer_valid = true;
    runtime_capacity_guard_initialized = true;
    // PR #352: clear the warned-once latch on every (re-)initialize so a
    // subsequent failure can re-arm the one-shot ERR. See initialize() for
    // the rationale.
    failed_init_warning_emitted = false;
    invalid_camera_input_events = 0;
    last_invalid_camera_log_frame = UINT64_MAX;
    visibility.reset_runtime_state();
    diagnostics = DiagnosticsState();
    budget.pending_upload_bytes = 0;
    budget.pending_upload_slots = 0;
    budget.retired_upload_bytes_this_frame = 0;
    budget.retired_upload_slots_this_frame = 0;
    budget.failed_upload_retirements = 0;
    pending_upload_retirements.clear();
    next_upload_ticket_id = 1;
    last_completed_upload_ticket_id = 0;
    last_upload_completion_mode = "none";
    analytics_snapshot.clear();
    scheduler.visible_scan_cursor = 0;
    scheduler.prefetch_scan_cursor = 0;
    scheduler.last_visible_scan_count = 0;
    scheduler.last_visible_scan_budget_effective = 0;
    scheduler.last_load_candidate_count = 0;
    scheduler.last_primary_eviction_scan_count = 0;
    scheduler.last_primary_eviction_candidate_count = 0;
    scheduler.last_non_primary_scan_count = 0;
    scheduler.last_non_primary_eviction_candidate_count = 0;
    scheduler.last_prefetch_scan_count = 0;
    scheduler.last_prefetch_scan_budget_effective = 0;
    scheduler.last_prefetch_candidate_count = 0;
    scheduler.last_prefetch_upload_pending_skip_count = 0;
    scheduler.last_prefetch_enqueued_count = 0;
    scheduler.last_prefetch_enqueue_headroom_stall_count = 0;
    scheduler.last_sync_fallback_queue_depth = 0;
    scheduler.last_sync_fallback_enqueued_count = 0;
    scheduler.last_sync_fallback_drained_count = 0;
    scheduler.last_sync_fallback_dropped_count = 0;
    scheduler.last_sync_fallback_stalled_count = 0;
    scheduler.queue_pressure_candidate_scan_throttle_active = false;
    scheduler.queue_pressure_candidate_scan_throttle_queue_depth = 0;
    scheduler.force_sync_fallback_due_to_async_stall = false;
    scheduler.sync_fallback_chunk_load_queue.clear();
    scheduler.sync_fallback_chunk_load_set.clear();
    scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
    scheduler.last_update_cpu_ms = 0.0;
    scheduler.last_visibility_cpu_ms = 0.0;
    scheduler.last_load_cpu_ms = 0.0;
    scheduler.last_build_visible_cpu_ms = 0.0;
    scheduler.last_prefetch_cpu_ms = 0.0;
    scheduler.last_sync_fallback_cpu_ms = 0.0;
    scheduler.last_cpu_total_attributed_ms = 0.0;
    scheduler.last_cpu_unattributed_ms = 0.0;
    source_data.unref();
    per_chunk_quantization_dc_compatible = true;
    total_splat_count = 0;
    chunks.clear();
    eviction_controller.invalidate_resident_tracking();
    asset_registry.primary_chunk_source_indices.clear();
    primary_chunk_layout_metrics.reset();
    budget.evicted_bytes_total = 0;
    _register_primary_asset();

    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    RenderingDevice *rd = p_device ? p_device
            : (last_upload_device ? last_upload_device
                    : (manager ? manager->get_primary_rendering_device() : nullptr));
    last_upload_device = rd;
    global_atlas_registry.cleanup(rd);
    _release_persistent_buffer(rd, "initialize_empty");

    budget.vram_regulator.instantiate();
    budget.vram_regulator->initialize(rd);
    _apply_config_overrides();

    uint32_t effective_max_chunks = get_effective_max_chunks();
    const uint32_t addressable_max_chunks = _streaming_addressable_chunk_limit();
    if (addressable_max_chunks == 0) {
        if (!failed_init_warning_emitted) {
            ERR_PRINT("[Streaming] Empty initialization failed: chunk slot byte size is invalid for 32-bit RenderingDevice buffer addressing.");
            failed_init_warning_emitted = true;
        }
        streaming_initialized = false;
        return;
    }
    if (effective_max_chunks > addressable_max_chunks) {
        WARN_PRINT(vformat("[Streaming] Capping effective_max_chunks from %d to %d due to 32-bit buffer addressing limit.",
                effective_max_chunks, addressable_max_chunks));
        effective_max_chunks = addressable_max_chunks;
    }
    // Phase 3: empty system has no asset chunks yet; right-size to the minimum
    // floor so we don't reserve ~288 MiB before any data has been loaded.
    uint32_t initial_capacity = STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM;
    initial_capacity = MIN(initial_capacity, effective_max_chunks);
    if (rd) {
        const uint64_t persistent_bytes64 = uint64_t(initial_capacity) * _streaming_chunk_slot_bytes();
        if (persistent_bytes64 == 0 || persistent_bytes64 > uint64_t(UINT32_MAX)) {
            if (!failed_init_warning_emitted) {
                ERR_PRINT(vformat("[Streaming] Empty initialization failed: persistent buffer size overflow (%s bytes, max=%u).",
                        String::num_uint64(persistent_bytes64), UINT32_MAX));
                failed_init_warning_emitted = true;
            }
            persistent_buffer = RID();
            persistent_buffer_size = 0;
        } else {
            persistent_buffer_size = static_cast<uint32_t>(persistent_bytes64);
            persistent_buffer = rd->storage_buffer_create(persistent_buffer_size);
            rd->set_resource_name(persistent_buffer, "GS_Streaming_PersistentBuffer");
        }
    }

    atlas_allocator.reset(initial_capacity);
    streaming_initial_capacity = initial_capacity;
    streaming_current_capacity = initial_capacity;
    streaming_max_capacity = effective_max_chunks;
    streaming_grow_count = 0;
    const uint32_t runtime_capacity_max = _compute_runtime_chunk_capacity_limit();
    const bool persistent_buffer_valid = persistent_buffer.is_valid() && persistent_buffer_size > 0;
    if (effective_max_chunks == 0 || runtime_capacity_max == 0 || !persistent_buffer_valid) {
        if (!failed_init_warning_emitted) {
            ERR_PRINT(vformat("[Streaming] Empty initialization failed: runtime not loadable (effective_max=%d, runtime_capacity=%d, persistent_buffer_valid=%s, device_present=%s).",
                    effective_max_chunks,
                    runtime_capacity_max,
                    persistent_buffer_valid ? "yes" : "no",
                    rd ? "yes" : "no"));
            failed_init_warning_emitted = true;
        }
        streaming_initialized = false;
        return;
    }
    streaming_initialized = true;

    _load_quantization_config_from_project_settings();
    _refresh_quantization_dc_compatibility();
    _load_streaming_tuning_config_from_project_settings();
    _reload_debug_logging_config();

    quantization_dirty = true;
    global_atlas_registry.build_cpu_state(*this);
    global_atlas_registry.sync_to_gpu(*this, rd);

    GS_LOG_STREAMING_INFO(vformat("[Streaming] Initialized empty system (VRAM budget: %d MB, initial slots: %d, max slots: %d)",
            budget.vram_regulator->get_config().budget_mb,
            streaming_initial_capacity, streaming_max_capacity));
}

void GaussianStreamingSystem::update_primary_asset_data(Ref<::GaussianData> p_data) {
    if (p_data.is_null()) {
        GS_LOG_STREAMING_WARN("[Streaming] update_primary_asset_data called with null data");
        return;
    }

    if (!streaming_initialized) {
        GS_LOG_STREAMING_WARN("[Streaming] update_primary_asset_data called before system initialized");
        return;
    }

    upload_pipeline.cancel_asset_jobs(*this, PRIMARY_ASSET_ID);
    for (uint32_t i = 0; i < chunks.size(); i++) {
        StreamingChunk &chunk = chunks[i];
        if (chunk.buffer_slot != UINT32_MAX &&
                !(chunk.upload_pending && _has_pending_upload_retirement(PRIMARY_ASSET_ID, i, chunk.buffer_slot))) {
            atlas_allocator.release_slot(_make_chunk_key(PRIMARY_ASSET_ID, i));
            chunk.buffer_slot = UINT32_MAX;
        }
        if (chunk.is_loaded) {
            if (budget.loaded_chunks_count > 0) {
                budget.loaded_chunks_count--;
            }
            const uint64_t chunk_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
            const uint64_t evicted_bytes = budget.vram_usage > chunk_bytes ? chunk_bytes : budget.vram_usage;
            budget.vram_usage = budget.vram_usage > chunk_bytes ? (budget.vram_usage - chunk_bytes) : 0;
            budget.evicted_bytes_total += evicted_bytes;
        }
        chunk.is_loaded = false;
        chunk.upload_pending = false;
    }

    source_data = p_data;
    total_splat_count = source_data->get_count();
    _load_quantization_config_from_project_settings();
    if (!_create_chunks()) {
        ERR_PRINT("[Streaming] update_primary_asset_data failed: strict primary layout validation rejected chunk layout hints; disabling streaming runtime.");
        streaming_initialized = false;
        return;
    }

    // Update PRIMARY_ASSET_ID state in asset_registry.atlas_assets
    AtlasAssetState *asset = _get_asset_state(PRIMARY_ASSET_ID);
    if (asset) {
        asset->data = p_data;
        asset->sh_degree = p_data->get_sh_degree();
        asset->bounds = p_data->get_aabb();
        asset->metadata_dirty = true;
        asset->generation = _advance_asset_generation(PRIMARY_ASSET_ID);
    }
    _refresh_quantization_dc_compatibility();

    visibility.clear_visible_state();
    scheduler.visible_scan_cursor = 0;
    scheduler.prefetch_scan_cursor = 0;
    scheduler.sync_fallback_chunk_load_queue.clear();
    scheduler.sync_fallback_chunk_load_set.clear();
    scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
    scheduler.last_sync_fallback_queue_depth = 0;
    scheduler.force_sync_fallback_due_to_async_stall = false;
    frame_data[current_frame_idx].visible_chunks.clear();

    if (is_per_chunk_quantization_enabled()) {
        quantization_dirty = true;
    }
    quantization_cpu_cache_valid = false;
    global_atlas_registry.mark_asset_registry_dirty();
}

void GaussianStreamingSystem::attach_memory_stream(const Ref<GaussianMemoryStream> &p_stream) {
    memory_stream_proxy = p_stream;
    analytics_snapshot.clear();
}

void GaussianStreamingSystem::set_config_overrides(const ConfigOverrides &p_overrides) {
    config_overrides = p_overrides;
    config_overrides_active = p_overrides.has_any_override();
    config_dirty = true;
    eviction_controller.invalidate_candidate_cache();
}

void GaussianStreamingSystem::clear_config_overrides() {
    config_overrides = ConfigOverrides();
    config_overrides_active = false;
    config_dirty = true;
    eviction_controller.invalidate_candidate_cache();
}

void GaussianStreamingSystem::set_io_chunk_layout_hints(const Vector<ChunkLayoutHint> &p_hints, uint32_t p_asset_id) {
    asset_registry.io_chunk_layout_hints = p_hints;
    eviction_controller.invalidate_resident_tracking();
    if (asset_registry.io_chunk_layout_hints.is_empty()) {
        asset_registry.io_chunk_layout_asset_id = INVALID_ASSET_ID;
        return;
    }

    // Keep hints bound to a deterministic asset across frames to avoid
    // nondeterministic consumption in multi-asset registration order.
    asset_registry.io_chunk_layout_asset_id = p_asset_id;
}

void GaussianStreamingSystem::set_primary_chunk_layout(const Vector<ChunkLayoutHint> &p_hints, const Vector<uint32_t> &p_source_indices) {
    bool hints_changed = asset_registry.primary_chunk_layout_hints.size() != p_hints.size();
    if (!hints_changed) {
        for (int i = 0; i < p_hints.size(); i++) {
            const ChunkLayoutHint &prev = asset_registry.primary_chunk_layout_hints[i];
            const ChunkLayoutHint &next = p_hints[i];
            if (prev.start_idx != next.start_idx ||
                    prev.count != next.count ||
                    prev.source_index_offset != next.source_index_offset ||
                    prev.source_indices_remapped != next.source_indices_remapped ||
                    prev.bounds.position != next.bounds.position ||
                    prev.bounds.size != next.bounds.size ||
                    prev.center != next.center ||
                    !Math::is_equal_approx(prev.radius, next.radius)) {
                hints_changed = true;
                break;
            }
        }
    }

    bool indices_changed = asset_registry.primary_chunk_layout_source_indices.size() != static_cast<uint32_t>(p_source_indices.size());
    if (!indices_changed) {
        for (uint32_t i = 0; i < p_source_indices.size(); i++) {
            if (asset_registry.primary_chunk_layout_source_indices[i] != p_source_indices[i]) {
                indices_changed = true;
                break;
            }
        }
    }

    if (!hints_changed && !indices_changed) {
        return;
    }
    eviction_controller.invalidate_resident_tracking();

    asset_registry.primary_chunk_layout_hints = p_hints;
    asset_registry.primary_chunk_layout_source_indices.clear();
    asset_registry.primary_chunk_layout_source_indices.resize(p_source_indices.size());
    for (uint32_t i = 0; i < p_source_indices.size(); i++) {
        asset_registry.primary_chunk_layout_source_indices[i] = p_source_indices[i];
    }

    if (streaming_initialized) {
        if (source_data.is_valid()) {
            update_primary_asset_data(source_data);
        } else {
            AtlasAssetState *primary_asset = _get_asset_state(PRIMARY_ASSET_ID);
            if (primary_asset && primary_asset->payload_source.is_valid() && primary_asset->payload_source->is_valid()) {
                _create_chunks();
                visibility.clear_visible_state();
                primary_asset = _get_asset_state(PRIMARY_ASSET_ID);
                if (primary_asset) {
                    primary_asset->uses_primary_chunks = true;
                    primary_asset->metadata_dirty = true;
                    primary_asset->generation = _advance_asset_generation(PRIMARY_ASSET_ID);
                }
                quantization_dirty = true;
                quantization_cpu_cache_valid = false;
                global_atlas_registry.mark_asset_registry_dirty();
            }
        }
    }
}

void GaussianStreamingSystem::begin_residency_requests() {
    asset_registry.request_generation++;
    if (asset_registry.request_generation == 0) {
        asset_registry.request_generation = 1;
    }
    for (uint32_t asset_id : asset_registry.atlas_asset_order) {
        AtlasAssetState *asset = _get_asset_state(asset_id);
        if (!asset) {
            continue;
        }
        asset->requested_chunks.clear();
    }
    asset_registry.request_collection_active = true;
    asset_registry.request_pending = false;
}

Error GaussianStreamingSystem::request_chunk_residency(uint32_t asset_id, uint32_t chunk_id, uint32_t lod_level) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset) {
        ERR_PRINT(vformat("[Streaming] request_chunk_residency ignored: asset %d is not registered.", asset_id));
        return ERR_DOES_NOT_EXIST;
    }
    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    if (chunk_id >= asset_chunks.size()) {
        ERR_PRINT(vformat("[Streaming] request_chunk_residency ignored: asset %d chunk %d is out of range (chunk_count=%d).",
                asset_id, chunk_id, asset_chunks.size()));
        return ERR_INVALID_PARAMETER;
    }
    if (!asset_registry.request_collection_active) {
        begin_residency_requests();
    }
    const uint32_t safe_lod = MIN(lod_level, MAX_REQUESTED_LOD);
    RequestedChunkState &state = asset->requested_chunk_state[chunk_id];
    if (state.stamp != asset_registry.request_generation) {
        state.stamp = asset_registry.request_generation;
        state.lod_mask = 0;
        asset->requested_chunks.push_back(chunk_id);
    }
    state.request_generation = asset_registry.request_generation;
    state.request_state = GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_COLLECTED;
    state.request_result = GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_COLLECTED;
    state.request_error = OK;
    state.lod_mask |= (1u << safe_lod);
    asset_registry.request_pending = true;
    return OK;
}

Error GaussianStreamingSystem::request_asset_residency(uint32_t asset_id, uint32_t lod_level) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset) {
        ERR_PRINT(vformat("[Streaming] request_asset_residency ignored: asset %d is not registered.", asset_id));
        return ERR_DOES_NOT_EXIST;
    }
    if (!asset->data.is_valid() && !asset->payload_source.is_valid()) {
        ERR_PRINT(vformat("[Streaming] request_asset_residency ignored: asset %d has no data or payload source.", asset_id));
        return ERR_DOES_NOT_EXIST;
    }
    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    if (asset_chunks.is_empty()) {
        ERR_PRINT(vformat("[Streaming] request_asset_residency ignored: asset %d has zero chunks.", asset_id));
        return ERR_DOES_NOT_EXIST;
    }
    const uint32_t safe_lod = MIN(lod_level, MAX_REQUESTED_LOD);
    Error last_error = OK;
    for (uint32_t i = 0; i < asset_chunks.size(); i++) {
        const Error err = request_chunk_residency(asset_id, i, safe_lod);
        if (err != OK && last_error == OK) {
            last_error = err;
        }
    }
    return last_error;
}

void GaussianStreamingSystem::finalize_residency_requests() {
    asset_registry.request_collection_active = false;
}

Dictionary GaussianStreamingSystem::get_residency_request_status(uint32_t asset_id, uint32_t chunk_id) const {
    return _build_residency_request_status(asset_id, chunk_id);
}

bool GaussianStreamingSystem::_is_requested_chunk_in_current_generation(const AtlasAssetState &asset, uint32_t chunk_id) const {
    const RequestedChunkState *state = asset.requested_chunk_state.getptr(chunk_id);
    return state && state->stamp == asset_registry.request_generation;
}

void GaussianStreamingSystem::_update_requested_chunk_state(
        AtlasAssetState &asset, uint32_t chunk_id, uint8_t request_state, uint8_t request_result, Error request_error) {
    const RequestedChunkState *state = asset.requested_chunk_state.getptr(chunk_id);
    if (!state || state->stamp == 0) {
        return;
    }
    _update_requested_chunk_state_for_generation(
            asset,
            chunk_id,
            state->stamp,
            request_state,
            request_result,
            request_error);
}

void GaussianStreamingSystem::_update_requested_chunk_state_for_generation(
        AtlasAssetState &asset, uint32_t chunk_id, uint64_t request_generation,
        uint8_t request_state, uint8_t request_result, Error request_error) {
    RequestedChunkState *state = asset.requested_chunk_state.getptr(chunk_id);
    if (!state || state->stamp == 0 || state->stamp != request_generation) {
        return;
    }
    state->request_state = request_state;
    state->request_result = request_result;
    state->request_generation = request_generation;
    state->request_error = request_error;
}

StringName GaussianStreamingSystem::_residency_request_state_name(uint8_t request_state) {
    switch (request_state) {
        case GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_COLLECTED:
            return StringName("collected");
        case GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED:
            return StringName("queued");
        case GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED:
            return StringName("deferred");
        case GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED:
            return StringName("satisfied");
        case GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED:
            return StringName("failed");
        case GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE:
        default:
            return StringName("idle");
    }
}

StringName GaussianStreamingSystem::_residency_request_error_name(Error request_error) {
    switch (request_error) {
        case OK:
            return StringName("ok");
        case ERR_BUSY:
            return StringName("busy");
        case ERR_UNAVAILABLE:
            return StringName("unavailable");
        case ERR_DOES_NOT_EXIST:
            return StringName("does_not_exist");
        case ERR_INVALID_PARAMETER:
            return StringName("invalid_parameter");
        case FAILED:
            return StringName("failed");
        default:
            return StringName("unknown");
    }
}

bool GaussianStreamingSystem::_is_terminal_residency_request_error(Error request_error) {
    switch (request_error) {
        case ERR_BUSY:
        case OK:
            return false;
        case ERR_UNAVAILABLE:
        case ERR_DOES_NOT_EXIST:
        case ERR_INVALID_PARAMETER:
        case FAILED:
        default:
            return true;
    }
}

Dictionary GaussianStreamingSystem::_build_residency_request_status(uint32_t asset_id, uint32_t chunk_id) const {
    Dictionary status;
    status["asset_id"] = static_cast<int64_t>(asset_id);
    status["chunk_id"] = static_cast<int64_t>(chunk_id);
    status["request_generation"] = static_cast<int64_t>(asset_registry.request_generation);
    status["request_collection_active"] = asset_registry.request_collection_active;
    status["request_pending"] = asset_registry.request_pending;
    status["request_generation_current"] = int64_t(0);
    status["request_status_generation"] = int64_t(0);
    status["request_status_current_generation"] = false;
    status["request_error"] = int64_t(OK);
    status["request_error_name"] = _residency_request_error_name(OK);
    status["lod_mask"] = int64_t(0);
    status["stale_request_generation"] = int64_t(0);
    status["stale_request_state"] = static_cast<int64_t>(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE);
    status["stale_request_state_name"] = _residency_request_state_name(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE);
    status["stale_request_result"] = static_cast<int64_t>(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE);
    status["stale_request_result_name"] = _residency_request_state_name(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE);
    status["stale_request_error"] = int64_t(OK);
    status["stale_request_error_name"] = _residency_request_error_name(OK);

    const AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset) {
        status["requested"] = false;
        status["request_state"] = static_cast<int64_t>(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        status["request_state_name"] = _residency_request_state_name(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        status["request_result"] = static_cast<int64_t>(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        status["request_result_name"] = _residency_request_state_name(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        return status;
    }

    const LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    if (chunk_id >= asset_chunks.size()) {
        status["requested"] = false;
        status["request_state"] = static_cast<int64_t>(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        status["request_state_name"] = _residency_request_state_name(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        status["request_result"] = static_cast<int64_t>(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        status["request_result_name"] = _residency_request_state_name(GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED);
        return status;
    }

    const RequestedChunkState *state = asset->requested_chunk_state.getptr(chunk_id);
    const bool requested = _is_requested_chunk_in_current_generation(*asset, chunk_id);
    const bool has_recorded_status = state && state->request_generation != 0;
    const uint8_t request_state = has_recorded_status ? state->request_state : GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE;
    const uint8_t request_result = has_recorded_status ? state->request_result : GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_IDLE;
    const Error request_error = has_recorded_status ? static_cast<Error>(state->request_error) : OK;
    const bool stale_request = has_recorded_status && state->request_generation != asset_registry.request_generation;
    status["requested"] = requested;
    if (requested) {
        status["request_generation_current"] = static_cast<int64_t>(state->stamp);
        status["lod_mask"] = static_cast<int64_t>(state->lod_mask);
    }
    if (has_recorded_status) {
        status["request_status_generation"] = static_cast<int64_t>(state->request_generation);
        status["request_status_current_generation"] = state->request_generation == asset_registry.request_generation;
    }
    status["request_state"] = static_cast<int64_t>(request_state);
    status["request_state_name"] = _residency_request_state_name(request_state);
    status["request_result"] = static_cast<int64_t>(request_result);
    status["request_result_name"] = _residency_request_state_name(request_result);
    status["request_error"] = static_cast<int64_t>(request_error);
    status["request_error_name"] = _residency_request_error_name(request_error);
    if (stale_request) {
        status["stale_request_generation"] = static_cast<int64_t>(state->request_generation);
        status["stale_request_state"] = static_cast<int64_t>(request_state);
        status["stale_request_state_name"] = _residency_request_state_name(request_state);
        status["stale_request_result"] = static_cast<int64_t>(request_result);
        status["stale_request_result_name"] = _residency_request_state_name(request_result);
        status["stale_request_error"] = static_cast<int64_t>(request_error);
        status["stale_request_error_name"] = _residency_request_error_name(request_error);
    }
    return status;
}

void GaussianStreamingSystem::register_asset(uint32_t asset_id, const Ref<GaussianData> &p_data) {
    if (asset_id == PRIMARY_ASSET_ID || p_data.is_null()) {
        return;
    }

    const bool strict_layout_validation = _layout_hint_strict_validation_enabled();

    auto build_asset_chunks = [&](LocalVector<StreamingChunk> &out_chunks) -> bool {
        bool used_io_layout = false;
        const bool can_try_io_layout = config_overrides.override_io_source &&
                !asset_registry.io_chunk_layout_hints.is_empty() &&
                (asset_registry.io_chunk_layout_asset_id == INVALID_ASSET_ID || asset_registry.io_chunk_layout_asset_id == asset_id);
        if (can_try_io_layout) {
            used_io_layout = _build_chunks_from_layout_hints(p_data, asset_registry.io_chunk_layout_hints, out_chunks);
            if (used_io_layout) {
                asset_registry.io_chunk_layout_asset_id = asset_id;
            } else {
                const String fallback_message = _layout_hint_record_failure(
                        this,
                        LayoutHintUsage::IO,
                        vformat("io_asset_%d", asset_id),
                        strict_layout_validation);
                if (strict_layout_validation) {
                    ERR_PRINT(vformat("%s [strict_mode=enabled]", fallback_message));
                    return false;
                }
                WARN_PRINT(vformat("%s; falling back to contiguous chunk partitioning.", fallback_message));
            }
        }
        if (!used_io_layout) {
            _build_chunks_for_data(p_data, out_chunks);
        }
        return true;
    };

    AtlasAssetState *existing = _get_asset_state(asset_id);
    if (!existing) {
        LocalVector<StreamingChunk> initial_chunks;
        if (!build_asset_chunks(initial_chunks)) {
            ERR_FAIL_MSG(vformat("[Streaming] Strict layout hint validation rejected IO chunk layout for asset %d.", asset_id));
        }

        const uint32_t next_generation = _advance_asset_generation(asset_id);
        AtlasAssetState asset;
        asset.asset_id = asset_id;
        asset.data = p_data;
        asset.uses_primary_chunks = false;
        asset.lod_count = 1;
        asset.sh_degree = p_data->get_sh_degree();
        asset.bounds = p_data->get_aabb();
        asset.metadata_dirty = true;
        asset.generation = next_generation;
        asset.asset_chunks = std::move(initial_chunks);
        asset.dense_id = _alloc_dense_id(asset_id);
        asset_registry.atlas_assets.insert(asset_id, asset);
        asset_registry.atlas_asset_order.push_back(asset_id);
    } else {
        LocalVector<StreamingChunk> rebuilt_chunks;
        if (!build_asset_chunks(rebuilt_chunks)) {
            ERR_FAIL_MSG(vformat("[Streaming] Strict layout hint validation rejected IO chunk layout for asset %d.", asset_id));
        }
        const uint32_t next_generation = _advance_asset_generation(asset_id);
        upload_pipeline.cancel_asset_jobs(*this, asset_id);
        LocalVector<StreamingChunk> &existing_chunks = _get_asset_chunks(*existing);
        for (uint32_t i = 0; i < existing_chunks.size(); i++) {
            StreamingChunk &chunk = existing_chunks[i];
            if (chunk.buffer_slot != UINT32_MAX &&
                    !(chunk.upload_pending && _has_pending_upload_retirement(asset_id, i, chunk.buffer_slot))) {
                atlas_allocator.release_slot(_make_chunk_key(asset_id, i));
                chunk.buffer_slot = UINT32_MAX;
            }
            if (chunk.is_loaded) {
                if (budget.loaded_chunks_count > 0) {
                    budget.loaded_chunks_count--;
                }
                const uint64_t chunk_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
                const uint64_t evicted_bytes = budget.vram_usage > chunk_bytes ? chunk_bytes : budget.vram_usage;
                budget.vram_usage = budget.vram_usage > chunk_bytes ? (budget.vram_usage - chunk_bytes) : 0;
                budget.evicted_bytes_total += evicted_bytes;
            }
            chunk.is_loaded = false;
            chunk.upload_pending = false;
        }
        if (existing->dense_id == INVALID_ASSET_ID) {
            existing->dense_id = _alloc_dense_id(asset_id);
        }
        asset_registry.asset_id_to_dense[asset_id] = existing->dense_id;
        existing->data = p_data;
        existing->uses_primary_chunks = false;
        existing->lod_count = 1;
        existing->sh_degree = p_data->get_sh_degree();
        existing->bounds = p_data->get_aabb();
        existing->requested_chunks.clear();
        existing->requested_chunk_state.clear();
        existing->metadata_dirty = true;
        existing->asset_chunks = std::move(rebuilt_chunks);
        existing->generation = next_generation;
    }

    if (per_chunk_quantization_enabled) {
        quantization_dirty = true;
    }
    quantization_cpu_cache_valid = false;
    _refresh_quantization_dc_compatibility();
    eviction_controller.invalidate_resident_tracking();

    global_atlas_registry.mark_asset_registry_dirty();
}

void GaussianStreamingSystem::unregister_asset(uint32_t asset_id) {
    if (asset_id == PRIMARY_ASSET_ID) {
        return;
    }

    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset) {
        return;
    }

    asset->generation = _advance_asset_generation(asset_id);
    upload_pipeline.cancel_asset_jobs(*this, asset_id);

    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    for (uint32_t i = 0; i < asset_chunks.size(); i++) {
        StreamingChunk &chunk = asset_chunks[i];
        if (chunk.buffer_slot != UINT32_MAX &&
                !(chunk.upload_pending && _has_pending_upload_retirement(asset_id, i, chunk.buffer_slot))) {
            atlas_allocator.release_slot(_make_chunk_key(asset_id, i));
            chunk.buffer_slot = UINT32_MAX;
        }
        if (chunk.is_loaded) {
            if (budget.loaded_chunks_count > 0) {
                budget.loaded_chunks_count--;
            }
            const uint64_t chunk_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
            const uint64_t evicted_bytes = budget.vram_usage > chunk_bytes ? chunk_bytes : budget.vram_usage;
            budget.vram_usage = budget.vram_usage > chunk_bytes ? (budget.vram_usage - chunk_bytes) : 0;
            budget.evicted_bytes_total += evicted_bytes;
        }
        chunk.is_loaded = false;
        chunk.upload_pending = false;
    }

    const uint32_t dense_id = asset->dense_id;
    asset_registry.atlas_assets.erase(asset_id);
    for (uint32_t i = 0; i < asset_registry.atlas_asset_order.size(); i++) {
        if (asset_registry.atlas_asset_order[i] == asset_id) {
            asset_registry.atlas_asset_order.remove_at(i);
            break;
        }
    }
    asset_registry.asset_id_to_dense.erase(asset_id);
    _release_dense_id(dense_id);
    if (asset_registry.io_chunk_layout_asset_id == asset_id) {
        asset_registry.io_chunk_layout_asset_id = INVALID_ASSET_ID;
    }

    global_atlas_registry.mark_asset_registry_dirty();
    quantization_dirty = true;
    quantization_cpu_cache_valid = false;
    _refresh_quantization_dc_compatibility();
    eviction_controller.invalidate_resident_tracking();
}

void GaussianStreamingSystem::set_chunk_payload_source(uint32_t asset_id, const Ref<ChunkPayloadSource> &p_source) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    ERR_FAIL_NULL_MSG(asset, vformat("[Streaming] set_chunk_payload_source: asset %d not registered.", asset_id));
    asset->payload_source = p_source;
    if (asset_id == PRIMARY_ASSET_ID && p_source.is_valid() && p_source->is_valid()) {
        if (source_data.is_null()) {
            total_splat_count = p_source->get_count();
        }
        asset->uses_primary_chunks = true;
        asset->sh_degree = p_source->get_sh_degree();
        asset->bounds = p_source->get_bounds();
        asset->metadata_dirty = true;
        asset->generation = _advance_asset_generation(PRIMARY_ASSET_ID);
        if (source_data.is_null()) {
            _create_chunks();
            visibility.clear_visible_state();
            asset = _get_asset_state(asset_id);
            if (asset) {
                asset->payload_source = p_source;
                asset->uses_primary_chunks = true;
                asset->sh_degree = p_source->get_sh_degree();
                asset->bounds = p_source->get_bounds();
                asset->metadata_dirty = true;
            }
        }
        quantization_dirty = true;
        quantization_cpu_cache_valid = false;
        global_atlas_registry.mark_asset_registry_dirty();
    }
}

void GaussianStreamingSystem::detach_source_data(uint32_t asset_id) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    ERR_FAIL_NULL_MSG(asset, vformat("[Streaming] detach_source_data: asset %d not registered.", asset_id));

    if (asset->payload_source.is_null()) {
        WARN_PRINT(vformat("[Streaming] detach_source_data: asset %d has no payload source; refusing to detach.", asset_id));
        return;
    }
    asset->data.unref();

    // For the primary asset, also release the system-level source_data reference.
    if (asset_id == PRIMARY_ASSET_ID) {
        source_data.unref();
    }
}

void GaussianStreamingSystem::_refresh_quantization_dc_compatibility() {
    bool compatible = true;
    for (const KeyValue<uint32_t, AtlasAssetState> &E : asset_registry.atlas_assets) {
        if (!_data_has_uniform_dc_encoding(E.value.data)) {
            compatible = false;
            break;
        }
    }

    if (per_chunk_quantization_dc_compatible == compatible) {
        return;
    }

    per_chunk_quantization_dc_compatible = compatible;
    quantization_dirty = true;
    quantization_cpu_cache_valid = false;
    global_atlas_registry.mark_asset_registry_dirty();
    if (!compatible) {
        WARN_PRINT_ONCE("[Quantization] Disabled per-chunk quantization for mixed DC-encoding assets; using the non-quantized upload path.");
    }
}

uint32_t GaussianStreamingSystem::get_dense_asset_id(uint32_t asset_id) const {
    if (const uint32_t *dense = asset_registry.asset_id_to_dense.getptr(asset_id)) {
        return *dense;
    }
    return INVALID_ASSET_ID;
}

bool GaussianStreamingSystem::remap_instance_asset_ids(LocalVector<InstanceDataGPU> &p_instances, bool p_warn_on_missing) const {
    bool ok = true;
    for (uint32_t i = 0; i < p_instances.size(); i++) {
        const uint32_t incoming_asset_id = p_instances[i].ids[0];
        const uint32_t incoming_dense_generation = p_instances[i].lod[1];
        uint32_t dense_id = INVALID_ASSET_ID;
        uint32_t dense_generation = 0;

        if (const uint32_t *dense_from_external = asset_registry.asset_id_to_dense.getptr(incoming_asset_id)) {
            dense_id = *dense_from_external;
            dense_generation = _get_dense_generation(dense_id);
        } else if (incoming_asset_id < asset_registry.dense_to_asset_id.size()) {
            const uint32_t mapped_asset = asset_registry.dense_to_asset_id[incoming_asset_id];
            const uint32_t mapped_generation = _get_dense_generation(incoming_asset_id);
            if (mapped_asset != INVALID_ASSET_ID &&
                    mapped_generation != 0 &&
                    incoming_dense_generation == mapped_generation) {
                dense_id = incoming_asset_id;
                dense_generation = mapped_generation;
            } else {
                ok = false;
                if (p_warn_on_missing) {
                    WARN_PRINT_ONCE(vformat("[Streaming] Stale dense asset mapping detected (dense_id=%u, generation=%u); rejecting instance mapping.",
                            incoming_asset_id, incoming_dense_generation));
                }
            }
        } else {
            ok = false;
            if (p_warn_on_missing) {
                WARN_PRINT_ONCE(vformat("[Streaming] Instance asset_id %u is not registered; rejecting instance mapping.", incoming_asset_id));
            }
        }

        if (dense_id == INVALID_ASSET_ID || dense_generation == 0) {
            continue;
        }

        p_instances[i].ids[0] = dense_id;
        p_instances[i].lod[1] = dense_generation;
    }
    return ok;
}

bool GaussianStreamingSystem::_create_chunks() {
    asset_registry.primary_chunk_source_indices.clear();
    primary_chunk_layout_metrics.reset();
    const bool strict_layout_validation = _layout_hint_strict_validation_enabled();
    AtlasAssetState *primary_asset = _get_asset_state(PRIMARY_ASSET_ID);
    const Ref<ChunkPayloadSource> primary_payload_source = primary_asset ? primary_asset->payload_source : Ref<ChunkPayloadSource>();
    bool built_from_primary_layout = false;
	if (!asset_registry.primary_chunk_layout_hints.is_empty()) {
        built_from_primary_layout = _build_primary_chunks_from_layout_hints(
                source_data,
                asset_registry.primary_chunk_layout_hints,
                asset_registry.primary_chunk_layout_source_indices,
                chunks);
        if (!built_from_primary_layout) {
            const String fallback_message = _layout_hint_record_failure(
                    this,
                    LayoutHintUsage::PRIMARY,
                    "primary_asset",
                    strict_layout_validation);
            if (strict_layout_validation) {
                ERR_PRINT(vformat("%s [strict_mode=enabled]", fallback_message));
                return false;
            }
            WARN_PRINT(vformat("%s; falling back to runtime contiguous chunk partitioning.", fallback_message));
        }
    }
    if (!built_from_primary_layout) {
        if (source_data.is_valid()) {
            _build_chunks_for_data(source_data, chunks);
        } else {
            _build_chunks_for_payload_source(primary_payload_source, chunks);
        }
    }
    _refresh_primary_chunk_layout_metrics();
    eviction_controller.invalidate_resident_tracking();
    GS_LOG_STREAMING_INFO(vformat(
            "[Streaming] Created %d chunks with precomputed AABBs for frustum culling (spatial=%s avg_radius_ratio=%.4f volume_ratio=%.4f)",
            chunks.size(),
            primary_chunk_layout_metrics.spatial_partition_enabled ? "yes" : "no",
            primary_chunk_layout_metrics.avg_chunk_radius_ratio,
            primary_chunk_layout_metrics.bounds_volume_ratio));
    return true;
}

bool GaussianStreamingSystem::_build_primary_chunks_from_layout_hints(const Ref<GaussianData> &p_data,
        const Vector<ChunkLayoutHint> &p_hints, const LocalVector<uint32_t> &p_source_indices, LocalVector<StreamingChunk> &out_chunks) {
    out_chunks.clear();
    asset_registry.primary_chunk_source_indices.clear();
    AtlasAssetState *primary_asset = _get_asset_state(PRIMARY_ASSET_ID);
    const Ref<ChunkPayloadSource> primary_payload_source = primary_asset ? primary_asset->payload_source : Ref<ChunkPayloadSource>();
    const bool has_resident_data = p_data.is_valid();
    const bool has_payload_source = primary_payload_source.is_valid() && primary_payload_source->is_valid();
    if (!has_resident_data && !has_payload_source) {
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                LayoutHintFailureReason::DATA_NULL, -1, 0, 0 });
        return false;
    }
    if (p_hints.is_empty()) {
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                LayoutHintFailureReason::HINTS_EMPTY, -1, 0, 0 });
        return false;
    }
    if (p_source_indices.is_empty()) {
        const uint32_t expected_count = has_resident_data ? p_data->get_count() : primary_payload_source->get_count();
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                LayoutHintFailureReason::REMAP_SOURCE_COUNT_MISMATCH, -1, 0, static_cast<uint64_t>(expected_count) });
        return false;
    }

    const uint32_t splat_count = has_resident_data ? p_data->get_count() : primary_payload_source->get_count();
    if (splat_count == 0) {
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                LayoutHintFailureReason::SPLAT_COUNT_ZERO, -1, 0, 0 });
        return false;
    }

    LayoutHintValidationFailure validation_failure;
    uint64_t total_hint_count = 0;
    uint64_t required_chunk_count = 0;
    bool saw_oversized_hint = false;
    if (!_validate_layout_hint_ranges(
                p_hints,
                p_source_indices.size(),
                true,
                true,
                false,
                false,
                validation_failure,
                total_hint_count,
                required_chunk_count,
                saw_oversized_hint)) {
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, validation_failure);
        return false;
    }
    (void)required_chunk_count;
    (void)saw_oversized_hint;
    if (total_hint_count != p_source_indices.size()) {
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                LayoutHintFailureReason::REMAP_TOTAL_COUNT_MISMATCH, -1, total_hint_count, p_source_indices.size() });
        return false;
    }
    if (p_source_indices.size() != splat_count) {
        _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                LayoutHintFailureReason::REMAP_SOURCE_COUNT_MISMATCH, -1, p_source_indices.size(), splat_count });
        return false;
    }

    Vector<uint8_t> seen;
    seen.resize(splat_count);
    for (int i = 0; i < seen.size(); i++) {
        seen.write[i] = 0;
    }
    for (uint32_t i = 0; i < p_source_indices.size(); i++) {
        const uint32_t source_idx = p_source_indices[i];
        if (source_idx >= splat_count) {
            _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                    LayoutHintFailureReason::REMAP_SOURCE_INDEX_OUT_OF_RANGE, static_cast<int>(i), source_idx, splat_count });
            return false;
        }
        if (seen[source_idx] != 0) {
            _layout_hint_set_last_failure(this, LayoutHintUsage::PRIMARY, LayoutHintValidationFailure{
                    LayoutHintFailureReason::REMAP_SOURCE_INDEX_DUPLICATE, static_cast<int>(i), source_idx, 0 });
            return false;
        }
        seen.write[source_idx] = 1;
    }

    _layout_hint_clear_last_failure(this, LayoutHintUsage::PRIMARY);
    asset_registry.primary_chunk_source_indices.resize(p_source_indices.size());
    for (uint32_t i = 0; i < p_source_indices.size(); i++) {
        asset_registry.primary_chunk_source_indices[i] = p_source_indices[i];
    }
    out_chunks.resize(p_hints.size());
    const bool quantize = per_chunk_quantization_enabled && has_resident_data;
    if (per_chunk_quantization_enabled && !has_resident_data) {
        WARN_PRINT_ONCE("[Streaming] Per-chunk quantization is deferred for file-backed primary worlds; using unquantized chunk payloads.");
    }

    for (int i = 0; i < p_hints.size(); i++) {
        const ChunkLayoutHint &hint = p_hints[i];
        StreamingChunk &chunk = out_chunks[i];
        chunk.start_idx = hint.source_index_offset;
        chunk.count = hint.count;
        chunk.source_index_remapped = true;
        chunk.is_loaded = false;
        chunk.is_visible = true;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
        chunk.quantization_computed = false;
        chunk.effective_count = chunk.count;
        chunk.center = hint.center;
        chunk.bounds = hint.bounds;
        chunk.max_radius = MAX(hint.radius, chunk.bounds.size.length() * 0.5f);

        if (quantize) {
            LocalVector<Gaussian> remapped_chunk_gaussians;
            remapped_chunk_gaussians.resize(chunk.count);
            for (uint32_t local_idx = 0; local_idx < chunk.count; local_idx++) {
                const uint64_t remap_idx = uint64_t(chunk.start_idx) + uint64_t(local_idx);
                const uint32_t source_index = asset_registry.primary_chunk_source_indices[remap_idx];
                remapped_chunk_gaussians[local_idx] = p_data->get_gaussian(source_index);
            }
            chunk.quantization.compute_from_gaussians(
                    remapped_chunk_gaussians,
                    0,
                    chunk.count,
                    quantization_position_bits,
                    quantization_scale_bits,
                    quantization_scales_enabled);
            chunk.quantization_computed = true;
        }
    }

    return true;
}

void GaussianStreamingSystem::_build_chunks_for_data(const Ref<GaussianData> &p_data, LocalVector<StreamingChunk> &out_chunks) {
    out_chunks.clear();
    if (p_data.is_null()) {
        return;
    }

    const bool build_primary_spatial = (&out_chunks == &chunks);

    // Fast path: re-hydrate chunk bookkeeping from import-time bake. Avoids
    // re-walking every splat (the dominant per-asset register cost). Schema
    // mismatch (different CHUNK_SIZE / corrupt blob) falls through to the
    // full rebuild below. When this build owns the primary spatial remap we
    // also require the bake to carry one; importers currently skip Morton
    // sort, so without this gate primary datasets would silently fall back to
    // contiguous chunks and lose culling/streaming locality.
    const bool bake_usable_for_primary =
            !build_primary_spatial || p_data->has_baked_primary_source_indices();
    if (p_data->has_baked_streaming_chunks() && p_data->get_baked_chunk_size() == CHUNK_SIZE &&
            bake_usable_for_primary) {
        if (_populate_chunks_from_bake(p_data, out_chunks, build_primary_spatial)) {
            if (build_primary_spatial) {
                const PackedInt32Array &baked_primary = p_data->get_streaming_primary_source_indices_raw();
                const uint32_t expected = p_data->get_count();
                const int n = baked_primary.size();
                // Mirror _populate_chunks_from_bake: only adopt the remap when
                // it covers the full splat range; a truncated remap would
                // alias tail splats and is rejected upstream.
                if (n > 0 && uint32_t(n) == expected) {
                    asset_registry.primary_chunk_source_indices.resize(uint32_t(n));
                    const int32_t *src = baked_primary.ptr();
                    for (int i = 0; i < n; i++) {
                        asset_registry.primary_chunk_source_indices[uint32_t(i)] = uint32_t(src[i]);
                    }
                }
            }
            return;
        }
    }

    const uint32_t splat_count = p_data->get_count();
    const uint32_t num_chunks = (splat_count + CHUNK_SIZE - 1) / CHUNK_SIZE;
    out_chunks.resize(num_chunks);
    const bool quantize = per_chunk_quantization_enabled;
    const LocalVector<Gaussian> *gaussians = quantize ? &p_data->get_gaussian_storage() : nullptr;

    // Skip Morton sort for very large datasets.  The O(N*log(N)) sort on
    // hundreds of MB followed by random-access bounds computation into
    // multi-GB gaussian storage causes stalls of 30+ seconds.  Contiguous
    // chunks are sufficient for the initial pass; proper spatial layout
    // arrives from static chunk hints via set_primary_chunk_layout().
    static constexpr uint32_t kMortonSortSplatThreshold = 2'000'000;
    const bool morton_sort_feasible = splat_count <= kMortonSortSplatThreshold;
    if (build_primary_spatial && splat_count > 0 && !morton_sort_feasible) {
        WARN_PRINT(vformat("[Streaming] Skipping Morton sort for %d splats (threshold=%d); using contiguous chunk layout.",
                splat_count, kMortonSortSplatThreshold));
    }
    if (build_primary_spatial && splat_count > 0 && morton_sort_feasible) {
        struct MortonPair {
            uint32_t morton = 0;
            uint32_t source_index = 0;
        };

        const AABB scene_bounds = p_data->get_aabb();
        LocalVector<MortonPair> morton_pairs;
        morton_pairs.resize(splat_count);
        for (uint32_t i = 0; i < splat_count; i++) {
            const Gaussian g = p_data->get_gaussian(i);
            morton_pairs[i].morton = _compute_morton_code_10(g.position, scene_bounds);
            morton_pairs[i].source_index = i;
        }
        std::sort(morton_pairs.ptr(), morton_pairs.ptr() + morton_pairs.size(),
                [](const MortonPair &a, const MortonPair &b) {
                    if (a.morton == b.morton) {
                        return a.source_index < b.source_index;
                    }
                    return a.morton < b.morton;
                });

        asset_registry.primary_chunk_source_indices.resize(splat_count);
        for (uint32_t i = 0; i < splat_count; i++) {
            asset_registry.primary_chunk_source_indices[i] = morton_pairs[i].source_index;
        }
    }

    for (uint32_t i = 0; i < num_chunks; i++) {
        StreamingChunk &chunk = out_chunks[i];
        chunk.start_idx = i * CHUNK_SIZE;
        chunk.count = MIN(CHUNK_SIZE, splat_count - chunk.start_idx);
        chunk.source_index_remapped = build_primary_spatial && morton_sort_feasible;
        if (chunk.source_index_remapped &&
                (uint64_t(chunk.start_idx) + uint64_t(chunk.count)) > asset_registry.primary_chunk_source_indices.size()) {
            WARN_PRINT_ONCE("[Streaming] Primary spatial remap index range is invalid; falling back to contiguous chunk reads.");
            chunk.source_index_remapped = false;
        }
        chunk.is_loaded = false;
        chunk.is_visible = true;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
        chunk.quantization_computed = false;
        chunk.effective_count = chunk.count;

        // Precompute chunk center and AABB from actual splat positions
        Vector3 center = Vector3();
        AABB bounds;
        bool bounds_initialized = false;
        float max_radius = 0.0f;
        LocalVector<Gaussian> remapped_chunk_gaussians;
        if (quantize && chunk.source_index_remapped && chunk.count > 0) {
            remapped_chunk_gaussians.resize(chunk.count);
        }

        for (uint32_t local_idx = 0; local_idx < chunk.count; local_idx++) {
            uint32_t source_index = chunk.start_idx + local_idx;
            if (chunk.source_index_remapped) {
                const uint64_t remap_idx = uint64_t(chunk.start_idx) + uint64_t(local_idx);
                source_index = asset_registry.primary_chunk_source_indices[remap_idx];
            }
            const Gaussian g = p_data->get_gaussian(source_index);
            if (!remapped_chunk_gaussians.is_empty()) {
                remapped_chunk_gaussians[local_idx] = g;
            }
            Vector3 pos = g.position;
            center += pos;

            // Compute splat radius from scale (conservative estimate)
            float radius = MAX(MAX(g.scale.x, g.scale.y), g.scale.z);
            max_radius = MAX(max_radius, radius);
            AABB splat_aabb(pos - Vector3(radius, radius, radius), Vector3(radius * 2, radius * 2, radius * 2));

            if (!bounds_initialized) {
                bounds = splat_aabb;
                bounds_initialized = true;
            } else {
                bounds = bounds.merge(splat_aabb);
            }
        }

        if (chunk.count > 0) {
            center /= float(chunk.count);
        }
        chunk.center = center;
        chunk.max_radius = max_radius;
        chunk.bounds = bounds;
        if (quantize && gaussians) {
            if (chunk.source_index_remapped && remapped_chunk_gaussians.size() == chunk.count) {
                chunk.quantization.compute_from_gaussians(
                        remapped_chunk_gaussians,
                        0,
                        chunk.count,
                        quantization_position_bits,
                        quantization_scale_bits,
                        quantization_scales_enabled);
                chunk.quantization_computed = true;
            } else {
                chunk.quantization.compute_from_gaussians(
                        *gaussians,
                        chunk.start_idx,
                        chunk.count,
                        quantization_position_bits,
                        quantization_scale_bits,
                        quantization_scales_enabled);
                chunk.quantization_computed = true;
            }
        }
    }
}

bool GaussianStreamingSystem::_populate_chunks_from_bake(const Ref<GaussianData> &p_data, LocalVector<StreamingChunk> &out_chunks, bool p_build_primary_spatial) {
    if (p_data.is_null()) {
        return false;
    }
    if (p_data->get_baked_chunk_size() != CHUNK_SIZE) {
        return false;
    }
    Vector<StreamingChunkBakeRecord> records;
    if (!StreamingChunkBakeIO::deserialize_records(p_data->get_streaming_chunk_records_raw(), records)) {
        return false;
    }

    const uint32_t splat_count = p_data->get_count();
    const uint32_t expected_num_chunks = (splat_count + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (uint32_t(records.size()) != expected_num_chunks) {
        return false;
    }

    const PackedInt32Array &baked_primary = p_data->get_streaming_primary_source_indices_raw();
    const bool has_baked_primary = !baked_primary.is_empty();
    // Only enable per-chunk source remap when the baked remap covers every
    // splat. A truncated/corrupt remap (e.g. crash mid-bake or stale schema)
    // would drop tail splats via failed _resolve_primary_chunk_source_index;
    // contiguous indices match the asset's storage order and are the safe
    // fallback the no-bake path produces.
    const bool baked_primary_size_matches = has_baked_primary && uint32_t(baked_primary.size()) == splat_count;
    if (has_baked_primary && !baked_primary_size_matches) {
        WARN_PRINT(vformat("[Streaming] Baked primary source remap length (%d) does not match splat count (%u); falling back to contiguous indices.",
                baked_primary.size(), splat_count));
    }
    const bool want_primary_remap = p_build_primary_spatial && baked_primary_size_matches;

    // Baked quantization reuse: only valid if (a) runtime requests it, (b) the
    // bake recorded matching params, (c) array sizes line up. Otherwise we
    // recompute below using compute_from_gaussians.
    const PackedByteArray &baked_quant_bytes = p_data->get_streaming_quantization_records_raw();
    const int baked_quant_count = baked_quant_bytes.size() / int(sizeof(ChunkQuantizationInfo));
    const bool baked_quant_size_matches = baked_quant_count == int(expected_num_chunks);
    const ChunkQuantizationInfo *baked_quant_ptr = baked_quant_size_matches
            ? reinterpret_cast<const ChunkQuantizationInfo *>(baked_quant_bytes.ptr())
            : nullptr;

    const bool quantize = per_chunk_quantization_enabled;
    const LocalVector<Gaussian> *gaussians_storage = quantize ? &p_data->get_gaussian_storage() : nullptr;

    out_chunks.resize(expected_num_chunks);
    for (uint32_t i = 0; i < expected_num_chunks; i++) {
        const StreamingChunkBakeRecord &rec = records[int(i)];
        StreamingChunk &chunk = out_chunks[i];
        chunk.start_idx = rec.start_idx;
        chunk.count = rec.count;
        chunk.source_index_remapped = want_primary_remap;
        chunk.is_loaded = false;
        chunk.is_visible = true;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
        chunk.quantization_computed = false;
        chunk.effective_count = chunk.count;
        chunk.center = rec.center;
        chunk.max_radius = rec.max_radius;
        chunk.bounds = rec.bounds;

        if (quantize && gaussians_storage) {
            bool reused = false;
            if (baked_quant_ptr) {
                const ChunkQuantizationInfo &src = baked_quant_ptr[i];
                if (src.position_bits == quantization_position_bits &&
                        src.scale_bits == quantization_scale_bits &&
                        src.scales_quantized == quantization_scales_enabled) {
                    chunk.quantization = src;
                    chunk.quantization_computed = true;
                    reused = true;
                }
            }
            if (!reused) {
                chunk.quantization.compute_from_gaussians(
                        *gaussians_storage,
                        chunk.start_idx,
                        chunk.count,
                        quantization_position_bits,
                        quantization_scale_bits,
                        quantization_scales_enabled);
                chunk.quantization_computed = true;
            }
        }
    }
    return true;
}

void GaussianStreamingSystem::_build_chunks_for_payload_source(const Ref<ChunkPayloadSource> &p_source, LocalVector<StreamingChunk> &out_chunks) {
    out_chunks.clear();
    if (p_source.is_null() || !p_source->is_valid()) {
        return;
    }

    const uint32_t splat_count = p_source->get_count();
    const uint32_t num_chunks = (splat_count + CHUNK_SIZE - 1) / CHUNK_SIZE;
    out_chunks.resize(num_chunks);

    const AABB source_bounds = p_source->get_bounds();
    const Vector3 center = source_bounds.position + source_bounds.size * 0.5f;
    const float radius = source_bounds.size.length() * 0.5f;
    if (per_chunk_quantization_enabled) {
        WARN_PRINT_ONCE("[Streaming] Per-chunk quantization is deferred for file-backed primary worlds without static chunk metadata.");
    }

    for (uint32_t i = 0; i < num_chunks; i++) {
        StreamingChunk &chunk = out_chunks[i];
        chunk.start_idx = i * CHUNK_SIZE;
        chunk.count = MIN(CHUNK_SIZE, splat_count - chunk.start_idx);
        chunk.source_index_remapped = false;
        chunk.is_loaded = false;
        chunk.is_visible = true;
        chunk.upload_pending = false;
        chunk.buffer_slot = UINT32_MAX;
        chunk.quantization_computed = false;
        chunk.effective_count = chunk.count;
        chunk.center = center;
        chunk.max_radius = radius;
        chunk.bounds = source_bounds;
    }
}

bool GaussianStreamingSystem::_build_chunks_from_layout_hints(const Ref<GaussianData> &p_data,
		const Vector<ChunkLayoutHint> &p_hints, LocalVector<StreamingChunk> &out_chunks) {
	out_chunks.clear();
	if (p_data.is_null()) {
		_layout_hint_set_last_failure(this, LayoutHintUsage::IO, LayoutHintValidationFailure{
				LayoutHintFailureReason::DATA_NULL, -1, 0, 0 });
		return false;
	}
	if (p_hints.is_empty()) {
		_layout_hint_set_last_failure(this, LayoutHintUsage::IO, LayoutHintValidationFailure{
				LayoutHintFailureReason::HINTS_EMPTY, -1, 0, 0 });
		return false;
	}
	const uint32_t splat_count = p_data->get_count();
	if (splat_count == 0) {
		_layout_hint_set_last_failure(this, LayoutHintUsage::IO, LayoutHintValidationFailure{
				LayoutHintFailureReason::SPLAT_COUNT_ZERO, -1, 0, 0 });
		return false;
	}

	LayoutHintValidationFailure validation_failure;
	uint64_t total_hint_count = 0;
	uint64_t required_chunk_count = 0;
	bool saw_oversized_hint = false;
	if (!_validate_layout_hint_ranges(
				p_hints,
				splat_count,
				false,
				false,
				true,
				true,
				validation_failure,
				total_hint_count,
				required_chunk_count,
				saw_oversized_hint)) {
		_layout_hint_set_last_failure(this, LayoutHintUsage::IO, validation_failure);
		return false;
	}
	if (total_hint_count != splat_count) {
		_layout_hint_set_last_failure(this, LayoutHintUsage::IO, LayoutHintValidationFailure{
				LayoutHintFailureReason::HINT_NON_CONTIGUOUS_COVERAGE, -1, total_hint_count, splat_count });
		return false;
	}

	_layout_hint_clear_last_failure(this, LayoutHintUsage::IO);
	out_chunks.resize(uint32_t(required_chunk_count));
	const bool quantize = per_chunk_quantization_enabled;
	const LocalVector<Gaussian> *gaussians = quantize ? &p_data->get_gaussian_storage() : nullptr;
	uint32_t out_idx = 0;

	for (int i = 0; i < p_hints.size(); i++) {
		const ChunkLayoutHint &hint = p_hints[i];
		uint32_t split_start_idx = hint.start_idx;
		uint32_t remaining = hint.count;
		while (remaining > 0) {
			const uint32_t split_chunk_count = MIN(CHUNK_SIZE, remaining);
			StreamingChunk &chunk = out_chunks[out_idx++];
			chunk.start_idx = split_start_idx;
			chunk.count = split_chunk_count;
			chunk.source_index_remapped = false;
			chunk.is_loaded = false;
			chunk.is_visible = true;
			chunk.upload_pending = false;
			chunk.buffer_slot = UINT32_MAX;
			chunk.effective_count = chunk.count;
			chunk.quantization_computed = false;

			if (hint.count <= CHUNK_SIZE) {
				chunk.center = hint.center;
				chunk.bounds = hint.bounds;
				chunk.max_radius = MAX(0.0f, hint.radius);
			} else {
				// Oversized hint entries must be split to fit single atlas slots.
				// Recompute bounds/center for each split to keep culling conservative.
				Vector3 center = Vector3();
				AABB bounds;
				bool bounds_initialized = false;
				float max_radius = 0.0f;
				const uint32_t end_idx = split_start_idx + split_chunk_count;

				for (uint32_t j = split_start_idx; j < end_idx; j++) {
					const Gaussian &g = p_data->get_gaussian(j);
					const Vector3 pos = g.position;
					center += pos;
					const float radius = MAX(MAX(g.scale.x, g.scale.y), g.scale.z);
					max_radius = MAX(max_radius, radius);
					AABB splat_aabb(pos - Vector3(radius, radius, radius), Vector3(radius * 2, radius * 2, radius * 2));

					if (!bounds_initialized) {
						bounds = splat_aabb;
						bounds_initialized = true;
					} else {
						bounds = bounds.merge(splat_aabb);
					}
				}

				if (split_chunk_count > 0) {
					center /= float(split_chunk_count);
				}
				chunk.center = center;
				chunk.bounds = bounds;
				chunk.max_radius = max_radius;
			}

			if (quantize && gaussians) {
				chunk.quantization.compute_from_gaussians(
						*gaussians,
						chunk.start_idx,
						chunk.count,
						quantization_position_bits,
						quantization_scale_bits,
						quantization_scales_enabled);
				chunk.quantization_computed = true;
			}

			split_start_idx += split_chunk_count;
			remaining -= split_chunk_count;
		}
	}

	if (saw_oversized_hint) {
		WARN_PRINT_ONCE("[Streaming] IO chunk layout hint exceeded CHUNK_SIZE and was split across atlas slots.");
	}

	return true;
}

bool GaussianStreamingSystem::_resolve_primary_chunk_source_index(const StreamingChunk &chunk,
        uint32_t p_offset_in_chunk, uint32_t &r_source_index) const {
    if (p_offset_in_chunk >= chunk.count) {
        return false;
    }
    if (!chunk.source_index_remapped) {
        const uint64_t source_idx = uint64_t(chunk.start_idx) + uint64_t(p_offset_in_chunk);
        if (source_data.is_valid() && source_idx >= source_data->get_count()) {
            return false;
        }
        r_source_index = static_cast<uint32_t>(source_idx);
        return true;
    }

    const uint64_t remap_idx = uint64_t(chunk.start_idx) + uint64_t(p_offset_in_chunk);
    if (remap_idx >= asset_registry.primary_chunk_source_indices.size()) {
        return false;
    }
    const uint32_t source_idx = asset_registry.primary_chunk_source_indices[remap_idx];
    if (source_data.is_valid() && source_idx >= static_cast<uint32_t>(source_data->get_count())) {
        return false;
    }
    r_source_index = source_idx;
    return true;
}

void GaussianStreamingSystem::_refresh_primary_chunk_layout_metrics() {
    primary_chunk_layout_metrics.reset();
    primary_chunk_layout_metrics.spatial_partition_enabled = !chunks.is_empty() &&
            chunks[0].source_index_remapped &&
            !asset_registry.primary_chunk_source_indices.is_empty();
    primary_chunk_layout_metrics.source_index_count = asset_registry.primary_chunk_source_indices.size();

    AtlasAssetState *primary_asset = _get_asset_state(PRIMARY_ASSET_ID);
    const bool has_payload_source = primary_asset &&
            primary_asset->payload_source.is_valid() &&
            primary_asset->payload_source->is_valid();
    if (chunks.is_empty() || (source_data.is_null() && !has_payload_source)) {
        return;
    }

    const AABB asset_bounds = source_data.is_valid() ? source_data->get_aabb() : primary_asset->payload_source->get_bounds();
    const Vector3 asset_half = asset_bounds.size * 0.5f;
    const float asset_radius = asset_half.length();
    const double asset_volume = double(MAX(asset_bounds.size.x, 0.0f)) *
            double(MAX(asset_bounds.size.y, 0.0f)) *
            double(MAX(asset_bounds.size.z, 0.0f));

    double weighted_radius_ratio_sum = 0.0;
    uint64_t weighted_radius_count = 0;
    float max_radius_ratio = 0.0f;
    double total_chunk_volume = 0.0;

    for (const StreamingChunk &chunk : chunks) {
        if (chunk.count == 0) {
            continue;
        }
        const Vector3 half = chunk.bounds.size * 0.5f;
        const float chunk_radius = half.length();
        float radius_ratio = 0.0f;
        if (asset_radius > 1e-6f) {
            radius_ratio = chunk_radius / asset_radius;
        }
        max_radius_ratio = MAX(max_radius_ratio, radius_ratio);
        weighted_radius_ratio_sum += double(radius_ratio) * double(chunk.count);
        weighted_radius_count += chunk.count;

        const double chunk_volume = double(MAX(chunk.bounds.size.x, 0.0f)) *
                double(MAX(chunk.bounds.size.y, 0.0f)) *
                double(MAX(chunk.bounds.size.z, 0.0f));
        total_chunk_volume += chunk_volume;
    }

    if (weighted_radius_count > 0) {
        primary_chunk_layout_metrics.avg_chunk_radius_ratio =
                float(weighted_radius_ratio_sum / double(weighted_radius_count));
    }
    primary_chunk_layout_metrics.max_chunk_radius_ratio = max_radius_ratio;
    if (asset_volume > 1e-12) {
        primary_chunk_layout_metrics.bounds_volume_ratio = float(total_chunk_volume / asset_volume);
    }
}

void GaussianStreamingSystem::_register_primary_asset() {
    asset_registry.atlas_assets.clear();
    asset_registry.atlas_asset_order.clear();
    asset_registry.asset_id_to_dense.clear();
    asset_registry.dense_to_asset_id.clear();
    asset_registry.dense_id_generation.clear();
    asset_registry.free_dense_ids.clear();
    asset_registry.asset_generation_tracker.clear();
    asset_registry.io_chunk_layout_asset_id = INVALID_ASSET_ID;
    quantization_cpu_cache_valid = false;

    AtlasAssetState primary;
    primary.asset_id = PRIMARY_ASSET_ID;
    primary.dense_id = PRIMARY_ASSET_ID;
    primary.data = source_data;
    primary.uses_primary_chunks = true;
    primary.lod_count = 1;
    primary.sh_degree = source_data.is_valid() ? source_data->get_sh_degree() : 0;
    primary.bounds = source_data.is_valid() ? source_data->get_aabb() : AABB();
    primary.metadata_dirty = true;
    primary.generation = _advance_asset_generation(PRIMARY_ASSET_ID);

    asset_registry.atlas_assets.insert(PRIMARY_ASSET_ID, primary);
    asset_registry.atlas_asset_order.push_back(PRIMARY_ASSET_ID);
    asset_registry.asset_id_to_dense[PRIMARY_ASSET_ID] = PRIMARY_ASSET_ID;
    asset_registry.dense_to_asset_id.push_back(PRIMARY_ASSET_ID);
    asset_registry.dense_id_generation.push_back(1);
    eviction_controller.invalidate_resident_tracking();
    global_atlas_registry.mark_asset_registry_dirty();
}

uint32_t GaussianStreamingSystem::_advance_asset_generation(uint32_t asset_id) {
    uint32_t next_generation = 1;
    if (const uint32_t *existing = asset_registry.asset_generation_tracker.getptr(asset_id)) {
        next_generation = *existing + 1;
        if (next_generation == 0) {
            next_generation = 1;
        }
    }
    asset_registry.asset_generation_tracker[asset_id] = next_generation;
    return next_generation;
}

uint32_t GaussianStreamingSystem::_alloc_dense_id(uint32_t asset_id) {
    uint32_t dense_id = INVALID_ASSET_ID;
    if (!asset_registry.free_dense_ids.is_empty()) {
        dense_id = asset_registry.free_dense_ids[asset_registry.free_dense_ids.size() - 1];
        asset_registry.free_dense_ids.resize(asset_registry.free_dense_ids.size() - 1);
        if (dense_id >= asset_registry.dense_to_asset_id.size()) {
            asset_registry.dense_to_asset_id.resize(dense_id + 1);
        }
        if (dense_id >= asset_registry.dense_id_generation.size()) {
            asset_registry.dense_id_generation.resize(dense_id + 1);
        }
        asset_registry.dense_to_asset_id[dense_id] = asset_id;
        uint32_t next_generation = asset_registry.dense_id_generation[dense_id] + 1;
        if (next_generation == 0) {
            next_generation = 1;
        }
        asset_registry.dense_id_generation[dense_id] = next_generation;
    } else {
        dense_id = asset_registry.dense_to_asset_id.size();
        asset_registry.dense_to_asset_id.push_back(asset_id);
        asset_registry.dense_id_generation.push_back(1);
    }
    asset_registry.asset_id_to_dense[asset_id] = dense_id;
    return dense_id;
}

void GaussianStreamingSystem::_release_dense_id(uint32_t dense_id) {
    if (dense_id == PRIMARY_ASSET_ID || dense_id == INVALID_ASSET_ID) {
        return;
    }
    if (dense_id >= asset_registry.dense_to_asset_id.size()) {
        return;
    }
    asset_registry.dense_to_asset_id[dense_id] = INVALID_ASSET_ID;
    asset_registry.free_dense_ids.push_back(dense_id);
}

uint32_t GaussianStreamingSystem::_get_dense_generation(uint32_t dense_id) const {
    if (dense_id >= asset_registry.dense_id_generation.size()) {
        return 0;
    }
    return asset_registry.dense_id_generation[dense_id];
}

GaussianStreamingSystem::AtlasAssetState *GaussianStreamingSystem::_get_asset_state(uint32_t asset_id) {
    return asset_registry.atlas_assets.getptr(asset_id);
}

const GaussianStreamingSystem::AtlasAssetState *GaussianStreamingSystem::_get_asset_state(uint32_t asset_id) const {
    return asset_registry.atlas_assets.getptr(asset_id);
}

LocalVector<GaussianStreamingSystem::StreamingChunk> &GaussianStreamingSystem::_get_asset_chunks(AtlasAssetState &asset) {
    return asset.uses_primary_chunks ? chunks : asset.asset_chunks;
}

const LocalVector<GaussianStreamingSystem::StreamingChunk> &GaussianStreamingSystem::_get_asset_chunks(const AtlasAssetState &asset) const {
    return asset.uses_primary_chunks ? chunks : asset.asset_chunks;
}

uint64_t GaussianStreamingSystem::_make_chunk_key(uint32_t asset_id, uint32_t chunk_id) const {
    return (uint64_t(asset_id) << 32) | uint64_t(chunk_id);
}

#if defined(TESTS_ENABLED)
void GaussianStreamingSystem::_test_mark_chunk_loaded_for_eviction(uint32_t p_asset_id, uint32_t p_chunk_id,
        bool p_visible, uint64_t p_last_loaded_frame, uint64_t p_last_used_frame, float p_distance) {
    AtlasAssetState *asset = _get_asset_state(p_asset_id);
    ERR_FAIL_NULL(asset);
    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    ERR_FAIL_UNSIGNED_INDEX(p_chunk_id, asset_chunks.size());

	StreamingChunk &chunk = asset_chunks[p_chunk_id];
	uint32_t buffer_slot = UINT32_MAX;
	ERR_FAIL_COND_MSG(!atlas_allocator.allocate_slot(_make_chunk_key(p_asset_id, p_chunk_id), buffer_slot),
			"[Streaming][Test] Failed to allocate atlas slot for synthetic eviction resident chunk.");
	if (!chunk.is_loaded) {
		budget.loaded_chunks_count++;
		budget.vram_usage += uint64_t(chunk.count) * sizeof(PackedGaussian);
	}
	chunk.is_loaded = true;
	chunk.gpu_resident = true;
	chunk.upload_pending = false;
	chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_GPU_RETIRED;
	chunk.is_visible = p_visible;
	chunk.buffer_slot = buffer_slot;
	chunk.last_loaded_frame = p_last_loaded_frame;
	chunk.last_used_frame = p_last_used_frame;
	chunk.distance = p_distance;
    eviction_controller.note_chunk_loaded(p_asset_id, p_chunk_id);
}
#endif

void GaussianStreamingSystem::set_chunk_radius_multiplier(float p_multiplier) {
    float clamped = CLAMP(p_multiplier, 1.0f, 16.0f);
    if (!Math::is_equal_approx(visibility.chunk_radius_multiplier, clamped)) {
        visibility.chunk_radius_multiplier = clamped;
    }
}

void GaussianStreamingSystem::update_streaming(const Transform3D &camera_transform, const Projection &projection, float frame_delta_seconds) {
    // PR #352: short-circuit when initialize() previously failed. Without this
    // guard, every frame re-entered the full pipeline below, the runtime
    // capacity check below re-emitted the same ERR, and headless runs without
    // a RenderingDevice produced a 602-event SEH crash cascade in
    // run_module_tests.py (see #352 work-package brief). The warned-once
    // latch is owned by initialize()/initialize_empty() so a successful
    // re-init re-arms the one-shot diagnostic.
    if (!streaming_initialized) {
        return;
    }

    // Extract camera position from camera-to-world transform
    Vector3 camera_pos = camera_transform.origin;
    const float resolved_frame_delta_seconds = _resolve_frame_delta_seconds(frame_delta_seconds);
    OS *os = OS::get_singleton();
    const uint64_t scheduler_start_usec = os ? os->get_ticks_usec() : 0;

    _log_streaming_telemetry();
    _reload_config_if_dirty();

    if (!_is_finite_transform3d(camera_transform) || !_is_finite_projection(projection)) {
        invalid_camera_input_events++;
        if (last_invalid_camera_log_frame == UINT64_MAX ||
                total_frame_count >= last_invalid_camera_log_frame + invalid_camera_log_interval_frames) {
            WARN_PRINT(vformat("[Streaming] update_streaming skipped due invalid camera/projection input (frame=%d, invalid_events=%d).",
                    total_frame_count, invalid_camera_input_events));
            last_invalid_camera_log_frame = total_frame_count;
        }
        return;
    }

    const uint32_t regulated_max = budget.get_effective_max_chunks();
    const uint32_t effective_max = get_effective_max_chunks();
    const uint32_t runtime_capacity_max = _compute_runtime_chunk_capacity_limit();
    const bool persistent_buffer_valid = persistent_buffer.is_valid() && persistent_buffer_size > 0;
    const bool runtime_ready = streaming_initialized &&
            budget.vram_regulator.is_valid() &&
            effective_max > 0 &&
            runtime_capacity_max > 0 &&
            persistent_buffer_valid;
    if (!runtime_ready) {
        // PR #352: gate this ERR on the shared warned-once latch. The dynamic
        // re-emit on field changes used to fire many times per session in
        // headless lanes (and contributed to the 602-event crash cascade);
        // for failed-init we want at most one diagnostic per session, and
        // initialize() re-arms the latch on a successful re-init.
        if (!failed_init_warning_emitted &&
                (!runtime_capacity_guard_logged ||
                        runtime_capacity_guard_effective_max != effective_max ||
                        runtime_capacity_guard_runtime_capacity != runtime_capacity_max ||
                        runtime_capacity_guard_buffer_valid != persistent_buffer_valid ||
                        runtime_capacity_guard_initialized != streaming_initialized)) {
            ERR_PRINT(vformat("[Streaming] update_streaming aborted: runtime not loadable (initialized=%s, effective_max=%d, runtime_capacity=%d, persistent_buffer_valid=%s, configured_max=%d).",
                    streaming_initialized ? "yes" : "no",
                    effective_max,
                    runtime_capacity_max,
                    persistent_buffer_valid ? "yes" : "no",
                    regulated_max));
            runtime_capacity_guard_logged = true;
            runtime_capacity_guard_effective_max = effective_max;
            runtime_capacity_guard_runtime_capacity = runtime_capacity_max;
            runtime_capacity_guard_buffer_valid = persistent_buffer_valid;
            runtime_capacity_guard_initialized = streaming_initialized;
            failed_init_warning_emitted = true;
        }
        return;
    }
    if (runtime_capacity_guard_logged) {
        runtime_capacity_guard_logged = false;
        runtime_capacity_guard_effective_max = UINT32_MAX;
        runtime_capacity_guard_runtime_capacity = UINT32_MAX;
        runtime_capacity_guard_buffer_valid = true;
        runtime_capacity_guard_initialized = true;
    }

    _run_streaming_frame_pipeline(camera_transform, projection, camera_pos, resolved_frame_delta_seconds,
            regulated_max, effective_max, runtime_capacity_max, os, scheduler_start_usec);
}

void GaussianStreamingSystem::_run_streaming_frame_pipeline(const Transform3D &camera_transform, const Projection &projection,
        const Vector3 &camera_pos, float resolved_frame_delta_seconds, uint32_t regulated_max,
        uint32_t effective_max, uint32_t runtime_capacity_max, OS *os, uint64_t scheduler_start_usec) {
    const auto sample_cpu_ms = [&](uint64_t p_start_usec) -> double {
        if (!os || p_start_usec == 0) {
            return 0.0;
        }
        const uint64_t now_usec = os->get_ticks_usec();
        if (now_usec < p_start_usec) {
            return 0.0;
        }
        return double(now_usec - p_start_usec) / 1000.0;
    };

    _update_camera_tracking(camera_pos, resolved_frame_delta_seconds);
    uint64_t phase_start_usec = os ? os->get_ticks_usec() : 0;
    _update_chunk_visibility(camera_transform, projection);
    scheduler.last_visibility_cpu_ms = sample_cpu_ms(phase_start_usec);
    _handle_zero_visible_chunk_recovery();
    // Update distance-based LOD parameters for all chunks (Octree-GS)
    _update_chunk_lod_parameters(camera_pos);

    // Update LOD blend factors for smooth transitions (LODGE technique)
    _update_chunk_lod_blend_factors(camera_pos);

    if (per_frame_counters_reset_for_streaming_update) {
        per_frame_counters_reset_for_streaming_update = false;
    } else {
        _reset_per_frame_counters();
    }

    // Get effective max chunks from regulator
    if (effective_max < regulated_max) {
        if (!effective_max_guard_warning_emitted ||
                effective_max_guard_warning_regulated != regulated_max ||
                effective_max_guard_warning_capacity != runtime_capacity_max) {
            WARN_PRINT(vformat("[Streaming] Clamping effective max chunks from %d to %d (runtime capacity=%d).",
                    regulated_max, effective_max, runtime_capacity_max));
            effective_max_guard_warning_emitted = true;
            effective_max_guard_warning_regulated = regulated_max;
            effective_max_guard_warning_capacity = runtime_capacity_max;
        }
    } else if (effective_max_guard_warning_emitted) {
        effective_max_guard_warning_emitted = false;
        effective_max_guard_warning_regulated = 0;
        effective_max_guard_warning_capacity = 0;
    }

    uint32_t evictions_left = 0;
    bool eviction_blocked = false;
    phase_start_usec = os ? os->get_ticks_usec() : 0;
    _evict_for_vram_budget(evictions_left, eviction_blocked);
    uint32_t pack_queue_depth = 0;
    uint32_t upload_queue_depth = 0;
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_depth, upload_queue_depth);
    const bool can_async_pack = _can_use_async_pack_path(pack_queue_depth, upload_queue_depth);
    if (can_async_pack && _get_sync_fallback_queue_depth() > 0) {
        scheduler.sync_fallback_chunk_load_queue.clear();
        scheduler.sync_fallback_chunk_load_set.clear();
        scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
        scheduler.last_sync_fallback_queue_depth = 0;
    }
    if (can_async_pack) {
        _load_visible_chunks(effective_max, evictions_left, eviction_blocked);
        _apply_requested_residency(can_async_pack);
    } else {
        // In sync fallback mode the per-frame sync cap is very small; prioritize
        // explicit residency requests before opportunistic visible loads.
        _apply_requested_residency(can_async_pack);
        _load_visible_chunks(effective_max, evictions_left, eviction_blocked);
        const uint64_t sync_drain_start_usec = os ? os->get_ticks_usec() : 0;
        _drain_sync_fallback_chunk_loads(effective_max, evictions_left, eviction_blocked);
        scheduler.last_sync_fallback_cpu_ms = sample_cpu_ms(sync_drain_start_usec);
    }
    scheduler.last_load_cpu_ms = sample_cpu_ms(phase_start_usec);

    if (upload_pipeline.async_pack_enabled) {
        _process_upload_queue();
    }
    _process_upload_retirements();

    phase_start_usec = os ? os->get_ticks_usec() : 0;
    _build_visible_chunk_list();
    scheduler.last_build_visible_cpu_ms = sample_cpu_ms(phase_start_usec);
    phase_start_usec = os ? os->get_ticks_usec() : 0;
    _handle_predictive_prefetch(camera_pos, effective_max);
    scheduler.last_prefetch_cpu_ms = sample_cpu_ms(phase_start_usec);
    _update_vram_regulator();
    _log_streaming_frame_stats(effective_max);

    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    RenderingDevice *rd = primary_device_override ? primary_device_override
            : (last_upload_device ? last_upload_device
                    : (manager ? manager->get_primary_rendering_device() : nullptr));
    if (!rd) {
        WARN_PRINT_ONCE("[Streaming DIAG] update_streaming has no RenderingDevice; atlas GPU sync is deferred.");
    }
    scheduler.last_update_cpu_ms = sample_cpu_ms(scheduler_start_usec);
    global_atlas_registry.sync_to_gpu(*this, rd);
}

void GaussianStreamingSystem::_log_streaming_telemetry() {
    static uint64_t debug_frame = 0;
    const bool telemetry_enabled = debug_logging_enabled;
    const uint32_t log_freq = debug_frame_log_frequency > 0 ? debug_frame_log_frequency : 60;
    if (!telemetry_enabled || (++debug_frame % log_freq) != 1) {
        return;
    }

    GS_LOG_STREAMING_DEBUG(vformat("[STREAM-DBG] frame=%d chunks=%d loaded=%d vis=%d",
            total_frame_count, chunks.size(), budget.loaded_chunks_count,
            frame_data[current_frame_idx].visible_chunks.size()));

    uint32_t pack_queue_size = 0;
    uint32_t upload_queue_size = 0;
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_size, upload_queue_size);

    const StreamingUploadPipeline::PackTelemetry::Snapshot tsnap = upload_pipeline.telemetry.read_current();

    const double pack_avg_ms = tsnap.pack_jobs > 0
            ? double(tsnap.pack_time_total) / (USEC_PER_MS * double(tsnap.pack_jobs))
            : 0.0;
    const double pack_max_ms = double(tsnap.pack_time_max) / USEC_PER_MS;
    const double upload_mb = double(tsnap.upload_bytes) / double(BYTES_PER_MB);
    const double upload_mb_per_frame = upload_mb / double(log_freq);

    GS_LOG_STREAMING_DEBUG(vformat(
            "[STREAM-PACK] frame=%d async=%s threads=%d inflight=%d pack_avg=%.2fms pack_max=%.2fms pack_jobs=%d "
            "upload=%.2fMB (%.2fMB/f) upload_chunks=%d queues=(pack=%d upload=%d)",
            total_frame_count,
            upload_pipeline.async_pack_enabled ? "yes" : "no",
            upload_pipeline.pack_worker_threads,
            upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed),
            pack_avg_ms,
            pack_max_ms,
            tsnap.pack_jobs,
            upload_mb,
            upload_mb_per_frame,
            tsnap.upload_chunks,
            pack_queue_size,
            upload_queue_size));
}

void GaussianStreamingSystem::_reload_config_if_dirty() {
    if (!config_dirty) {
        return;
    }

    _load_prefetch_config_from_project_settings();
    _load_lod_blend_config_from_project_settings();
    _load_lod_config_from_project_settings();
    _update_culling_config_from_project_settings();
    _load_zero_visible_recovery_config_from_project_settings();
    _load_streaming_tuning_config_from_project_settings();
    if (budget.vram_regulator.is_valid()) {
        budget.vram_regulator->reload_config();
    }
    _apply_config_overrides();
    _reload_debug_logging_config();
    last_config_reload_frame = total_frame_count;
    config_dirty = false;
}

float GaussianStreamingSystem::_resolve_frame_delta_seconds(float p_frame_delta_seconds) {
    OS *os = OS::get_singleton();
    if (Math::is_finite(p_frame_delta_seconds) && p_frame_delta_seconds > 0.0f) {
        last_streaming_frame_delta_seconds = CLAMP(p_frame_delta_seconds, 0.0005f, 0.25f);
        if (os) {
            last_streaming_update_usec = os->get_ticks_usec();
        }
        return last_streaming_frame_delta_seconds;
    }

    const uint64_t now_usec = os ? os->get_ticks_usec() : 0;
    if (now_usec > 0 && last_streaming_update_usec > 0 && now_usec >= last_streaming_update_usec) {
        const double measured_delta_seconds = double(now_usec - last_streaming_update_usec) / 1000000.0;
        if (Math::is_finite(measured_delta_seconds) && measured_delta_seconds > 0.0) {
            last_streaming_frame_delta_seconds = CLAMP(float(measured_delta_seconds), 0.0005f, 0.25f);
        }
    } else if (!Math::is_finite(last_streaming_frame_delta_seconds) || last_streaming_frame_delta_seconds <= 0.0f) {
        last_streaming_frame_delta_seconds = ESTIMATED_FRAME_DELTA_60FPS;
    }

    if (now_usec > 0) {
        last_streaming_update_usec = now_usec;
    }
    return last_streaming_frame_delta_seconds;
}

uint32_t GaussianStreamingSystem::_compute_runtime_chunk_capacity_limit() const {
    const uint32_t allocator_capacity_chunks = atlas_allocator.get_capacity();
    const uint64_t chunk_bytes = uint64_t(CHUNK_SIZE) * sizeof(PackedGaussian);
    const uint32_t buffer_capacity_chunks = chunk_bytes > 0
            ? static_cast<uint32_t>(uint64_t(persistent_buffer_size) / chunk_bytes)
            : 0;
    if (allocator_capacity_chunks == 0 || buffer_capacity_chunks == 0) {
        return 0;
    }
    return MIN(allocator_capacity_chunks, buffer_capacity_chunks);
}

uint64_t GaussianStreamingSystem::_get_auxiliary_vram_overhead_bytes() const {
    return uint64_t(quantization_buffer_size) +
            global_atlas_registry.get_auxiliary_vram_overhead_bytes();
}

uint64_t GaussianStreamingSystem::_get_total_vram_usage_bytes() const {
    // budget.vram_usage tracks the *loaded chunk payload* that currently lives
    // inside the persistent storage buffer. The persistent buffer itself is the
    // single largest streaming VRAM allocation (sized for the resident chunk set
    // plus growth headroom), and chunk payloads are uploaded into it — so the
    // reserved VRAM is the whole buffer allocation, not just the bytes uploaded
    // so far. Reporting only the payload under-counts the regulator's true
    // footprint by the unfilled remainder of the buffer. Count the full
    // allocation (MAX guards against any transient payload > size accounting
    // skew, and degrades to the payload when the buffer is not yet allocated).
    // Only count the allocation when the RID is actually live: initialize() and
    // initialize_empty() set persistent_buffer_size *before* the storage_buffer_create()
    // that may fail and leave an invalid RID with a stale nonzero size. Without this
    // gate get_vram_usage()/diagnostics would report a phantom allocation after a failed
    // init instead of degrading to the payload as documented. (Codex #411)
    const uint64_t allocated_persistent_bytes = persistent_buffer.is_valid() ? uint64_t(persistent_buffer_size) : 0;
    const uint64_t resident_buffer_bytes = MAX(budget.vram_usage, allocated_persistent_bytes);
    return resident_buffer_bytes + _get_auxiliary_vram_overhead_bytes();
}

uint64_t GaussianStreamingSystem::_get_evictable_vram_usage_bytes() const {
    // Eviction/regulation/admission decision basis: ONLY the bytes that evicting chunks can
    // actually reclaim. Evicting a chunk frees only its slot in the persistent buffer, i.e. it
    // reduces budget.vram_usage. It does NOT free either non-reclaimable fixed cost:
    //   - the persistent-buffer ALLOCATION (the resident_buffer term in _get_total_vram_usage_bytes), nor
    //   - the auxiliary atlas/quantization overhead (_get_auxiliary_vram_overhead_bytes).
    // Including EITHER here would, whenever that fixed cost is large relative to the budget,
    // keep the decision basis above threshold even after every chunk is evicted — recreating
    // the infinite-pressure / evict-every-visible-chunk loop with no VRAM actually freed. The
    // only quantity that always falls to zero as chunks are evicted is the payload itself, so
    // that is the reclaimable basis. Reporting (get_vram_usage / analytics / overlay) and the
    // budget warning use the full allocation-inclusive total; decisions use this. (Codex #411)
    return budget.vram_usage;
}

uint32_t GaussianStreamingSystem::_get_reserved_chunk_count() const {
    return budget.loaded_chunks_count + budget.pending_upload_slots;
}

uint64_t GaussianStreamingSystem::_get_pending_upload_bytes_for_diagnostics() const {
    return budget.pending_upload_bytes;
}

void GaussianStreamingSystem::_load_zero_visible_recovery_config_from_project_settings() {
    visibility.load_zero_visible_recovery_config_from_project_settings();
}

void GaussianStreamingSystem::_update_camera_tracking(const Vector3 &camera_pos, float p_frame_delta_seconds) {
    visibility.update_camera_tracking(camera_pos, p_frame_delta_seconds);
}

void GaussianStreamingSystem::_handle_zero_visible_chunk_recovery() {
    visibility.handle_zero_visible_chunk_recovery(*this);
}

void GaussianStreamingSystem::_reset_per_frame_counters() {
    budget.chunks_loaded_this_frame = 0;
    budget.vram_chunk_cap_hit_this_frame = false;
    budget.retired_upload_bytes_this_frame = 0;
    budget.retired_upload_slots_this_frame = 0;
    eviction_controller.reset_per_frame_counters();
    upload_pipeline.queued_chunk_loads_this_frame = 0;
    upload_pipeline.upload_frame_cap_hit_this_frame = false;
    upload_pipeline.upload_slice_cap_hit_this_frame = false;
    upload_pipeline.upload_bandwidth_cap_hit_this_frame = false;
    upload_pipeline.chunk_load_cap_hit_this_frame = false;
    StreamingQueuePressureController::reset_latched_state(
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason);
    scheduler.last_visible_scan_count = 0;
    scheduler.last_visible_scan_budget_effective = 0;
    scheduler.last_load_candidate_count = 0;
    scheduler.last_primary_eviction_scan_count = 0;
    scheduler.last_primary_eviction_candidate_count = 0;
    scheduler.last_non_primary_scan_count = 0;
    scheduler.last_non_primary_eviction_candidate_count = 0;
    scheduler.last_prefetch_scan_count = 0;
    scheduler.last_prefetch_scan_budget_effective = 0;
    scheduler.last_prefetch_candidate_count = 0;
    scheduler.last_prefetch_upload_pending_skip_count = 0;
    scheduler.last_prefetch_enqueued_count = 0;
    scheduler.last_prefetch_enqueue_headroom_stall_count = 0;
    scheduler.last_sync_fallback_enqueued_count = 0;
    scheduler.last_sync_fallback_drained_count = 0;
    scheduler.last_sync_fallback_dropped_count = 0;
    scheduler.last_sync_fallback_stalled_count = 0;
    scheduler.last_sync_fallback_cpu_ms = 0.0;
    scheduler.queue_pressure_candidate_scan_throttle_active = false;
    scheduler.queue_pressure_candidate_scan_throttle_queue_depth = 0;
    scheduler.last_cpu_total_attributed_ms = 0.0;
    scheduler.last_cpu_unattributed_ms = 0.0;
    scheduler.last_sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    scheduler.prefetch_loads_remaining_this_frame = scheduler.max_prefetch_loads_per_frame;
    scheduler.prefetch_scan_budget_remaining_this_frame = scheduler.max_prefetch_chunk_scan_per_frame;
}

void GaussianStreamingSystem::_evict_for_vram_budget(uint32_t &evictions_left, bool &eviction_blocked) {
    const uint32_t max_evictions_per_frame = eviction_controller.get_max_evictions_per_frame();
    evictions_left = max_evictions_per_frame == 0 ? UINT32_MAX : max_evictions_per_frame;
    eviction_blocked = false;
    bool force_visible_eviction = false;
    const uint64_t evictable_vram_usage = _get_evictable_vram_usage_bytes();

    if (budget.vram_regulator.is_valid()) {
        const VRAMBudgetConfig &budget_config = budget.vram_regulator->get_config();
        const uint64_t budget_bytes = uint64_t(budget_config.budget_mb) * BYTES_PER_MB;
        force_visible_eviction = budget_bytes > 0 && evictable_vram_usage > budget_bytes;
    }

    if (!budget.vram_regulator.is_valid() ||
            !budget.vram_regulator->should_trigger_eviction(evictable_vram_usage)) {
        return;
    }

    while (budget.loaded_chunks_count > 0 &&
            budget.vram_regulator->should_trigger_eviction(_get_evictable_vram_usage_bytes()) &&
            evictions_left > 0) {
        // Non-primary-first eviction keeps primary visible residency stable
        // under budget pressure. _evict_non_primary_lru() now returns
        // EvictionResult and does NOT internally record (matches
        // _evict_least_recently_used()'s contract), so we record here.
        EvictionResult non_primary_result = _evict_non_primary_lru();
        if (non_primary_result == EvictionResult::EvictedNonVisible ||
                non_primary_result == EvictionResult::EvictedVisible) {
            eviction_controller.record_eviction_result(non_primary_result);
            evictions_left--;
            continue;
        }

        EvictionResult result = _evict_least_recently_used(force_visible_eviction);
        if (result == EvictionResult::EvictedNonVisible || result == EvictionResult::EvictedVisible) {
            eviction_controller.record_eviction_result(result);
            evictions_left--;
            continue;
        }

        eviction_blocked = true;
        break;
    }
}

// Shared helper for chunk-load admission and atlas-slot scarcity only.
// VRAM-budget eviction stays on _evict_for_vram_budget() because it has
// separate budget accounting and fallback semantics.
GaussianStreamingSystem::EvictionResult GaussianStreamingSystem::_evict_for_admission_gate(
        const ResidencyBudgetController::AdmissionGate &p_admission_gate, bool &r_visible_fallback_attempted) {
    r_visible_fallback_attempted = false;

    // Prefer evicting a non-primary-asset chunk first under atlas-slot
    // pressure. `_evict_least_recently_used()` walks `system.chunks`
    // (the primary-asset chunk list), so without this preliminary pass an
    // atlas-slot shortage triggered by a non-primary-asset load would
    // either evict primary chunks or report SkippedAllVisible even when
    // non-primary chunks are available, stalling multi-asset streaming.
    // Mirrors the VRAM-budget path in `_evict_for_vram_budget()` (~line 2668).
    // _evict_non_primary_lru() returns the actual EvictionResult; propagate it
    // so callers record visible-vs-nonvisible counts correctly (the previous
    // hardcoded EvictedNonVisible under-counted visible evictions).
    EvictionResult non_primary_result = _evict_non_primary_lru();
    if (non_primary_result == EvictionResult::EvictedNonVisible ||
            non_primary_result == EvictionResult::EvictedVisible) {
        return non_primary_result;
    }

    EvictionResult result = _evict_least_recently_used(false);
    if (result == EvictionResult::SkippedAllVisible &&
            ResidencyBudgetController::should_attempt_visible_evict_fallback(p_admission_gate)) {
        r_visible_fallback_attempted = true;
        result = _evict_least_recently_used(true);
    }

    return result;
}

void GaussianStreamingSystem::_load_visible_chunks(uint32_t effective_max, uint32_t &evictions_left, bool &eviction_blocked) {
    // PR #352: short-circuit when initialize() previously failed so this
    // method never touches persistent_buffer / chunks[] from a half-built
    // runtime. The runtime_ready gate in update_streaming() already early-
    // returns in that case, but the brief calls for an entry-level guard
    // here too — _load_visible_chunks is reachable from
    // _run_streaming_frame_pipeline and the test surface, both of which can
    // be exercised independently.
    if (!streaming_initialized) {
        eviction_blocked = true;
        return;
    }

    const uint32_t runtime_capacity_max = _compute_runtime_chunk_capacity_limit();
    const bool persistent_buffer_valid = persistent_buffer.is_valid() && persistent_buffer_size > 0;
    if (effective_max == 0 || runtime_capacity_max == 0 || !persistent_buffer_valid) {
        // PR #352: gate on the warned-once latch so a regression that flips
        // a previously-ready system into a half-built state still produces a
        // single diagnostic instead of one per frame.
        if (!failed_init_warning_emitted &&
                (!runtime_capacity_guard_logged ||
                        runtime_capacity_guard_effective_max != effective_max ||
                        runtime_capacity_guard_runtime_capacity != runtime_capacity_max ||
                        runtime_capacity_guard_buffer_valid != persistent_buffer_valid ||
                        runtime_capacity_guard_initialized != streaming_initialized)) {
            ERR_PRINT(vformat("[Streaming] _load_visible_chunks aborted: runtime not loadable (initialized=%s, effective_max=%d, runtime_capacity=%d, persistent_buffer_valid=%s).",
                    streaming_initialized ? "yes" : "no",
                    effective_max,
                    runtime_capacity_max,
                    persistent_buffer_valid ? "yes" : "no"));
            runtime_capacity_guard_logged = true;
            runtime_capacity_guard_effective_max = effective_max;
            runtime_capacity_guard_runtime_capacity = runtime_capacity_max;
            runtime_capacity_guard_buffer_valid = persistent_buffer_valid;
            runtime_capacity_guard_initialized = streaming_initialized;
            failed_init_warning_emitted = true;
        }
        eviction_blocked = true;
        return;
    }

    const LocalVector<uint32_t> &visible_chunks = visibility.visible_chunk_indices;
    if (visible_chunks.is_empty()) {
        scheduler.last_visible_scan_count = 0;
        scheduler.last_visible_scan_budget_effective = 0;
        scheduler.last_load_candidate_count = 0;
        return;
    }

    const uint32_t visible_count = visible_chunks.size();
    uint32_t scan_budget = scheduler.max_visible_chunk_scan_per_frame == 0
            ? visible_count
            : MIN(visible_count, scheduler.max_visible_chunk_scan_per_frame);
    uint32_t pack_queue_depth = 0;
    uint32_t upload_queue_depth = 0;
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_depth, upload_queue_depth);
    const uint32_t sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    const uint32_t observed_throttle_queue_depth =
            MAX(sync_fallback_queue_depth, MAX(pack_queue_depth, upload_queue_depth));
    const bool can_async_pack = _can_use_async_pack_path(pack_queue_depth, upload_queue_depth);
    const bool prioritize_sync_visible_enqueue = !can_async_pack && sync_fallback_queue_depth > 0;
    uint32_t enqueue_headroom = UINT32_MAX;
    if (can_async_pack) {
        enqueue_headroom = _compute_async_enqueue_headroom(
                upload_pipeline.queued_chunk_loads_this_frame,
                upload_pipeline.max_chunk_loads_per_frame,
                upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed),
                upload_pipeline.max_pack_jobs_in_flight);
    }
    StreamingQueuePressureController::ScanBudgetInput throttle_input;
    throttle_input.base_scan_budget = scan_budget;
    throttle_input.throttle_enabled = scheduler.queue_pressure_candidate_scan_throttle_enabled;
    throttle_input.throttle_min_queue_depth = scheduler.queue_pressure_candidate_scan_throttle_min_queue_depth;
    throttle_input.observed_queue_depth = observed_throttle_queue_depth;
    throttle_input.throttle_scan_cap = scheduler.queue_pressure_candidate_scan_throttle_visible_scan_cap;
    throttle_input.scanned_this_frame = 0;
    throttle_input.enqueue_headroom = enqueue_headroom;
    const StreamingQueuePressureController::ScanBudgetResult throttle_result =
            StreamingQueuePressureController::compute_candidate_scan_budget(throttle_input);
    scan_budget = throttle_result.scan_budget;
    scheduler.queue_pressure_candidate_scan_throttle_active = throttle_result.throttle_active;
    scheduler.queue_pressure_candidate_scan_throttle_queue_depth =
            MAX(scheduler.queue_pressure_candidate_scan_throttle_queue_depth,
                    throttle_result.effective_queue_depth);
    scheduler.last_visible_scan_budget_effective = scan_budget;
    if (scan_budget == 0) {
        scheduler.last_visible_scan_count = 0;
        scheduler.last_visible_scan_budget_effective = 0;
        scheduler.last_load_candidate_count = 0;
        return;
    }
    if (scheduler.visible_scan_cursor >= visible_count) {
        scheduler.visible_scan_cursor = 0;
    }

    // Under a throttled scan budget (queue pressure or limited enqueue headroom)
    // the round-robin cursor can starve the nearest prefix: a later frame resumes
    // mid-list and enqueues farther chunks before the nearest ones. visible_chunks
    // is sorted nearest-first (streaming_visibility_controller.cpp:463), so when
    // the budget can't cover the full list we always restart at 0 to prefer the
    // near field. Only fall back to the cursor when the budget can scan the whole
    // visible list in one frame (the fairness case).
    const bool restart_from_nearest = scan_budget < visible_count;
    const uint32_t scan_origin = restart_from_nearest ? 0u : scheduler.visible_scan_cursor;

    ResidencyBudgetController::AdmissionFrameBudget admission_budget =
            ResidencyBudgetController::make_frame_budget(effective_max, evictions_left, eviction_blocked);
    const float lod_mult = budget.vram_regulator.is_valid()
            ? budget.vram_regulator->get_lod_distance_multiplier()
            : 1.0f;
    const float load_threshold = STREAMING_LOAD_DISTANCE_BASE / lod_mult;
    uint32_t load_candidates = 0;
    bool blocked_by_chunk_cap = false;

    uint32_t scanned_chunks = 0;
    // Complexity: bounded by the effective throttled scan budget under queue pressure.
    for (; scanned_chunks < scan_budget; scanned_chunks++) {
        if (can_async_pack && scanned_chunks > 0 &&
                _compute_async_enqueue_headroom(
                        upload_pipeline.queued_chunk_loads_this_frame,
                        upload_pipeline.max_chunk_loads_per_frame,
                        upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed),
                        upload_pipeline.max_pack_jobs_in_flight) == 0) {
            break;
        }
        const uint32_t visible_idx = (scan_origin + scanned_chunks) % visible_count;
        const uint32_t chunk_idx = visible_chunks[visible_idx];
        if (chunk_idx >= chunks.size()) {
            continue;
        }
        StreamingChunk &chunk = chunks[chunk_idx];
        if (chunk.distance >= load_threshold || chunk.is_loaded || chunk.upload_pending) {
            continue;
        }
        load_candidates++;

        ResidencyBudgetController::AdmissionPolicy admission_policy;
        // Sync fallback visible path can still make forward progress via drain-time replacement.
        admission_policy.can_replace_without_eviction = !can_async_pack;
        admission_policy.enforce_vram_regulator_gate = budget.vram_regulator.is_valid();
        const uint32_t reserved_chunks = _get_reserved_chunk_count();
        admission_policy.vram_regulator_allows_load =
                !admission_policy.enforce_vram_regulator_gate ||
                budget.vram_regulator->can_load_more_chunks(reserved_chunks);
        _try_grow_persistent_buffer_for_atlas_pressure(
                reserved_chunks,
                get_regulated_max_chunks(),
                admission_policy.enforce_vram_regulator_gate,
                admission_policy.vram_regulator_allows_load);
        admission_policy.atlas_slots_full = !atlas_allocator.has_free_slots();

        const ResidencyBudgetController::AdmissionGate admission_gate =
                ResidencyBudgetController::compute_admission_gate(
                        reserved_chunks,
                        admission_budget,
                        admission_policy);
        const ResidencyBudgetController::AdmissionDecision decision = admission_gate.decision;
        if (decision == ResidencyBudgetController::AdmissionDecision::Skip) {
            if (admission_gate.context.loaded_chunks >= admission_gate.context.effective_max ||
                    (admission_gate.context.enforce_vram_regulator_gate &&
                            !admission_gate.context.vram_regulator_allows_load)) {
                blocked_by_chunk_cap = true;
            }
            continue;
        }
        if (decision == ResidencyBudgetController::AdmissionDecision::EvictThenLoad) {
            blocked_by_chunk_cap = true;
            bool visible_fallback_attempted = false;
            EvictionResult result = _evict_for_admission_gate(admission_gate, visible_fallback_attempted);
            if (visible_fallback_attempted) {
                diagnostics.visible_evict_fallback_attempts++;
                if (result == EvictionResult::EvictedNonVisible || result == EvictionResult::EvictedVisible) {
                    diagnostics.visible_evict_fallback_successes++;
                }
            }
            if (result == EvictionResult::EvictedNonVisible || result == EvictionResult::EvictedVisible) {
                eviction_controller.record_eviction_result(result);
                ResidencyBudgetController::note_successful_eviction(admission_budget);
            } else {
                ResidencyBudgetController::note_blocked_eviction(admission_budget);
                continue;
            }
        }

        const bool queued = _enqueue_chunk_load_request(
                PRIMARY_ASSET_ID, chunk_idx, can_async_pack, prioritize_sync_visible_enqueue);
        if (!queued && !can_async_pack) {
            scheduler.last_sync_fallback_stalled_count++;
        }
    }
    // If we restarted from the nearest prefix this frame, the cursor stays where
    // it was so that a healthy (unthrottled) frame can resume fair round-robin.
    // Otherwise advance it by the scanned count.
    if (!restart_from_nearest) {
        scheduler.visible_scan_cursor = (scheduler.visible_scan_cursor + scanned_chunks) % visible_count;
    }
    scheduler.last_visible_scan_count = scanned_chunks;
    scheduler.last_visible_scan_budget_effective = scanned_chunks;
    scheduler.last_load_candidate_count = load_candidates;
    budget.vram_chunk_cap_hit_this_frame = blocked_by_chunk_cap;
    evictions_left = admission_budget.evictions_left;
    eviction_blocked = admission_budget.eviction_blocked;
}

void GaussianStreamingSystem::_build_visible_chunk_list() {
    FrameData &frame = frame_data[current_frame_idx];
    frame.visible_chunks.clear();

    const LocalVector<uint32_t> &visible_chunks = visibility.visible_chunk_indices;
    if (visible_chunks.is_empty()) {
        return;
    }

    const float lod_mult = budget.vram_regulator.is_valid()
            ? budget.vram_regulator->get_lod_distance_multiplier()
            : 1.0f;
    const float visible_threshold = STREAMING_LOAD_DISTANCE_BASE / lod_mult;

    for (uint32_t chunk_idx : visible_chunks) {
        if (chunk_idx >= chunks.size()) {
            continue;
        }
        StreamingChunk &chunk = chunks[chunk_idx];
        if (chunk.is_loaded && chunk.gpu_resident && chunk.distance < visible_threshold) {
            frame.visible_chunks.push_back(chunk_idx);
            eviction_controller.touch_chunk_use(chunk.last_used_frame);
        }
    }
}

void GaussianStreamingSystem::_handle_predictive_prefetch(const Vector3 &camera_pos, uint32_t effective_max) {
    if (!visibility.predictive_prefetch_enabled || !visibility.camera_tracker.has_previous_position) {
        return;
    }

    const float base_lookahead = MAX(0.0f, visibility.prefetch_lookahead_distance);
    if (base_lookahead <= 0.0f) {
        return;
    }

    uint32_t pack_queue_depth = 0;
    uint32_t upload_queue_depth = 0;
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_depth, upload_queue_depth);
    const bool can_async_pack = _can_use_async_pack_path(pack_queue_depth, upload_queue_depth);
    uint32_t frame_prefetch_budget = scheduler.prefetch_loads_remaining_this_frame;
    if (upload_pipeline.max_chunk_loads_per_frame > 0) {
        const uint32_t frame_loads_used = can_async_pack
                ? upload_pipeline.queued_chunk_loads_this_frame
                : budget.chunks_loaded_this_frame;
        const uint32_t frame_load_budget = frame_loads_used < upload_pipeline.max_chunk_loads_per_frame
                ? (upload_pipeline.max_chunk_loads_per_frame - frame_loads_used)
                : 0;
        frame_prefetch_budget = MIN(frame_prefetch_budget, frame_load_budget);
    }
    if (!can_async_pack) {
        const uint32_t sync_queue_depth = _get_sync_fallback_queue_depth();
        const uint32_t sync_drain_capacity = scheduler.max_sync_fallback_loads_per_frame;
        const uint32_t sync_prefetch_capacity = sync_queue_depth >= sync_drain_capacity
                ? 0
                : (sync_drain_capacity - sync_queue_depth);
        frame_prefetch_budget = MIN(frame_prefetch_budget, sync_prefetch_capacity);
    }

    upload_pipeline.get_pending_queue_depths_cached(pack_queue_depth, upload_queue_depth);
    const bool pack_inflight_saturated = upload_pipeline.max_pack_jobs_in_flight > 0 &&
            upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed) >= upload_pipeline.max_pack_jobs_in_flight;
    const bool visible_backlog_exists =
            scheduler.last_load_candidate_count > 0 || scheduler.last_sync_fallback_stalled_count > 0;
    const bool pack_upload_pressure_high = pack_queue_depth > 0 ||
            upload_queue_depth > 0 ||
            pack_inflight_saturated ||
            upload_pipeline.upload_frame_cap_hit_this_frame ||
            upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    if (visible_backlog_exists && pack_upload_pressure_high) {
        frame_prefetch_budget = 0;
    } else if (visible_backlog_exists && upload_pipeline.queue_pressure_active) {
        frame_prefetch_budget = MIN<uint32_t>(frame_prefetch_budget, 1u);
    }
    if (frame_prefetch_budget == 0) {
        return;
    }

    // Multi-horizon prefetch: near -> mid -> far. Near horizon gets first
    // access to per-frame queue budget, while farther horizons opportunistically
    // consume remaining capacity.
    static const float horizon_multipliers[] = { 0.5f, 1.0f, 2.0f };
    for (uint32_t horizon_idx = 0; horizon_idx < 3; horizon_idx++) {
        if (frame_prefetch_budget == 0) {
            break;
        }
        uint32_t horizon_scan_budget = UINT32_MAX;
        if (scheduler.max_prefetch_chunk_scan_per_frame > 0) {
            const uint32_t remaining_scan_budget = scheduler.prefetch_scan_budget_remaining_this_frame;
            if (remaining_scan_budget == 0) {
                break;
            }
            const uint32_t remaining_horizons = 3 - horizon_idx;
            horizon_scan_budget = (remaining_scan_budget + remaining_horizons - 1) / remaining_horizons;
        }
        const uint32_t in_flight = _get_reserved_chunk_count();
        const uint32_t available_slots = in_flight < effective_max ? (effective_max - in_flight) : 0;
        const uint32_t load_budget = frame_prefetch_budget;
        if (available_slots == 0 || load_budget == 0) {
            break;
        }

        const float horizon_lookahead = base_lookahead * horizon_multipliers[horizon_idx];
        Vector3 predicted_pos = visibility.camera_tracker.predict_position(camera_pos, horizon_lookahead);
        const uint32_t queued_prefetch = _prefetch_chunks_at_predicted_position(
                predicted_pos, available_slots, load_budget, horizon_scan_budget);
        if (queued_prefetch == 0) {
            continue;
        }
        scheduler.last_prefetch_enqueued_count += queued_prefetch;
        frame_prefetch_budget = queued_prefetch >= frame_prefetch_budget
                ? 0
                : (frame_prefetch_budget - queued_prefetch);
        scheduler.prefetch_loads_remaining_this_frame =
                queued_prefetch >= scheduler.prefetch_loads_remaining_this_frame
                ? 0
                : (scheduler.prefetch_loads_remaining_this_frame - queued_prefetch);
    }
}

void GaussianStreamingSystem::_update_vram_regulator() {
    if (!budget.vram_regulator.is_valid()) {
        return;
    }

    // Feed the regulator BOTH bases: the allocation-inclusive total for reporting (debug
    // overlay + budget warning, matching get_vram_usage()) and the reclaimable/evictable
    // figure for decisions (admission gate + auto-regulation). Evicting/regulating frees only
    // budget.vram_usage, never the persistent-buffer allocation, so decisions must not gate on
    // the non-reclaimable allocation (it would deny loads / ratchet the cap down while no VRAM
    // can be freed); but the overlay must still show the true footprint. (Codex #411)
    budget.vram_regulator->update(_get_total_vram_usage_bytes(), _get_evictable_vram_usage_bytes(),
            budget.loaded_chunks_count, budget.chunks_loaded_this_frame,
            eviction_controller.get_chunks_evicted_this_frame(), total_frame_count);
}

void GaussianStreamingSystem::_log_streaming_frame_stats(uint32_t effective_max) {
    uint32_t visible_count = get_visible_count();
    visibility.prev_visible_count = visible_count;
    uint32_t effective_splats = get_effective_splat_count();

    if (visible_count == 0) {
        return;
    }

    const FrameData &frame = frame_data[current_frame_idx];
    if (debug_logging_enabled && debug_frame_log_frequency > 0 &&
            (total_frame_count % static_cast<uint64_t>(debug_frame_log_frequency) == 0)) {
        GS_LOG_STREAMING_DEBUG(vformat("[Streaming] Frame %d: %d visible chunks, %d visible splats (max: %d, usage: %.1f%%)",
                current_frame_idx, frame.visible_chunks.size(), visible_count,
                effective_max,
                budget.vram_regulator.is_valid() ? budget.vram_regulator->get_debug_stats().usage_percent : 0.0f));
    }

    static uint64_t lod_log_counter = 0;
    const LODConfig &lod_config = _get_lod_config();
    if (lod_config.enabled && lod_config.debug_visualization && (++lod_log_counter % 300) == 0) {
        float reduction_pct = visible_count > 0
                ? (1.0f - float(effective_splats) / float(visible_count)) * 100.0f
                : 0.0f;
        GS_LOG_STREAMING_INFO(vformat("[LOD] Splats: %d -> %d (%.1f%% reduction), Levels: %d, MaxDist: %.1f",
                visible_count, effective_splats, reduction_pct,
                lod_config.num_levels, lod_config.max_distance));
    }
}

void GaussianStreamingSystem::_update_chunk_visibility(const Transform3D &camera_transform, const Projection &projection) {
    visibility.update_chunk_visibility(*this, camera_transform, projection);
}

Error GaussianStreamingSystem::_load_chunk(uint32_t chunk_idx) {
    return _load_chunk(PRIMARY_ASSET_ID, chunk_idx);
}

void GaussianStreamingSystem::_assert_chunk_state_invariant(uint32_t asset_id, uint32_t chunk_idx,
        const StreamingChunk &chunk, const char *context,
        bool allow_deferred_allocator_release) {
    const uint64_t chunk_key = _make_chunk_key(asset_id, chunk_idx);
    if (_record_streaming_invariant(chunk.count == 0 || chunk.count > CHUNK_SIZE,
                diagnostics,
                diagnostics.invariant_upload_lifecycle_violations,
                context,
                vformat("[Streaming] Invalid chunk state (%s): asset=%d chunk=%d count=%d must be in [1, %d].",
                        context, asset_id, chunk_idx, chunk.count, CHUNK_SIZE))) {
        return;
    }
    if (chunk.upload_pending) {
        _validate_pending_upload_chunk_invariant(atlas_allocator, chunk, chunk_key,
                asset_id, chunk_idx, context, diagnostics);
        return;
    }

    if (chunk.is_loaded) {
        _validate_loaded_chunk_invariant(atlas_allocator, chunk, chunk_key,
                asset_id, chunk_idx, context, diagnostics);
        return;
    }

    // Some cancellation/error paths clear chunk state first and release allocator slot immediately after.
    _validate_idle_chunk_invariant(atlas_allocator, chunk, chunk_key, asset_id, chunk_idx,
            context, allow_deferred_allocator_release, diagnostics);
}

bool GaussianStreamingSystem::_begin_chunk_upload(uint32_t asset_id, uint32_t chunk_idx,
        StreamingChunk &chunk, uint32_t buffer_slot) {
    if (buffer_slot == UINT32_MAX || chunk.is_loaded || chunk.upload_pending) {
        return false;
    }

    const uint64_t chunk_key = _make_chunk_key(asset_id, chunk_idx);
    if (chunk.buffer_slot != UINT32_MAX) {
        // Defensive cleanup: stale slot assignment must not block new upload_pipeline.
        _release_chunk_slot_if_matches(atlas_allocator, chunk_key, chunk.buffer_slot);
        chunk.buffer_slot = UINT32_MAX;
    }

    uint32_t mapped_slot = UINT32_MAX;
    ERR_FAIL_COND_V_MSG(!_chunk_slot_matches_allocator(atlas_allocator, chunk_key, buffer_slot, &mapped_slot),
            false,
            vformat("[Streaming] Begin upload rejected: allocator slot mismatch asset=%d chunk=%d expected=%d mapped=%d.",
                    asset_id, chunk_idx, buffer_slot, mapped_slot == UINT32_MAX ? -1 : int(mapped_slot)));

    chunk.buffer_slot = buffer_slot;
    chunk.upload_pending = true;
    chunk.gpu_resident = false;
    chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_CPU_PACKED;
    chunk.upload_completion_mode = GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_NONE;
    chunk.upload_ticket_id = 0;
    chunk.upload_submit_frame = 0;
    chunk.upload_retire_frame = 0;
    chunk.pending_upload_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
    budget.pending_upload_bytes += chunk.pending_upload_bytes;
    budget.pending_upload_slots++;
    global_atlas_registry.mark_chunk_meta_dirty(*this, asset_id, chunk_idx);
    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_begin_chunk_upload");
    return true;
}

bool GaussianStreamingSystem::_stage_chunk_upload_retirement(uint32_t asset_id, uint32_t chunk_idx,
        StreamingChunk &chunk, uint32_t buffer_slot, uint64_t bytes,
        const SHCompressionMetrics &metrics, RenderingDevice *submission_rd,
        uint32_t override_retire_after_frames,
        StreamingUploadCompletionMode override_completion_mode) {
    if (!chunk.upload_pending || chunk.is_loaded || chunk.buffer_slot != buffer_slot ||
            buffer_slot == UINT32_MAX || bytes == 0) {
        _mark_chunk_upload_failed(asset_id, chunk_idx, chunk, "_stage_chunk_upload_retirement.invalid_state");
        _rollback_pending_chunk(asset_id, chunk_idx, chunk, true);
        return false;
    }

    const uint64_t ticket_id = next_upload_ticket_id++;
    uint8_t completion_mode = override_completion_mode;
    uint32_t retire_after_frames = override_retire_after_frames;
    if (completion_mode == GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_NONE) {
        if (submission_rd && !submission_rd->is_main_rendering_device()) {
            completion_mode = GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_LOCAL_RD_SUBMIT_SYNC;
            retire_after_frames = 0;
        } else {
            completion_mode = GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_MAIN_RD_FRAME_DELAY_BARRIER;
            retire_after_frames = _streaming_upload_frame_delay(submission_rd);
        }
    }
    if (retire_after_frames == UINT32_MAX) {
        retire_after_frames = completion_mode == GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_LOCAL_RD_SUBMIT_SYNC
                ? 0
                : _streaming_upload_frame_delay(submission_rd);
    }

    AtlasAssetState *asset = _get_asset_state(asset_id);
    PendingUploadRetirement ticket;
    ticket.ticket_id = ticket_id;
    ticket.asset_id = asset_id;
    ticket.chunk_idx = chunk_idx;
    ticket.buffer_slot = buffer_slot;
    ticket.asset_generation = asset ? asset->generation : 0;
    ticket.submit_frame = total_frame_count;
    ticket.retire_frame = total_frame_count + retire_after_frames;
    ticket.bytes = bytes;
    ticket.completion_mode = completion_mode;
    ticket.metrics = metrics;
    pending_upload_retirements.push_back(ticket);

    chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_GPU_RETIRE_PENDING;
    chunk.upload_completion_mode = completion_mode;
    chunk.upload_ticket_id = ticket_id;
    chunk.upload_submit_frame = ticket.submit_frame;
    chunk.upload_retire_frame = ticket.retire_frame;
    chunk.upload_device = submission_rd;
    last_upload_completion_mode = _streaming_upload_completion_mode_name(completion_mode);
    global_atlas_registry.mark_chunk_meta_dirty(*this, asset_id, chunk_idx);
    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_stage_chunk_upload_retirement");
    return true;
}

bool GaussianStreamingSystem::_has_pending_upload_retirement(uint32_t asset_id, uint32_t chunk_idx, uint32_t buffer_slot) const {
    for (uint32_t i = 0; i < pending_upload_retirements.size(); i++) {
        const PendingUploadRetirement &ticket = pending_upload_retirements[i];
        if (ticket.asset_id == asset_id && ticket.chunk_idx == chunk_idx &&
                (buffer_slot == UINT32_MAX || ticket.buffer_slot == buffer_slot)) {
            return true;
        }
    }
    return false;
}

void GaussianStreamingSystem::_mark_chunk_upload_failed(uint32_t asset_id, uint32_t chunk_idx,
        StreamingChunk &chunk, const char *context) {
    chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_FAILED_OR_ROLLED_BACK;
    budget.failed_upload_retirements++;
    diagnostics.invariant_upload_lifecycle_violations++;
    diagnostics.last_invariant_context = context ? context : "upload_failed";
    diagnostics.last_invariant_message = vformat(
            "[Streaming] Upload failed/rolled back before GPU retirement: asset=%d chunk=%d slot=%d state=%d.",
            asset_id, chunk_idx, chunk.buffer_slot == UINT32_MAX ? -1 : int(chunk.buffer_slot),
            int(chunk.upload_lifecycle_state));
}

void GaussianStreamingSystem::_rollback_pending_chunk(uint32_t asset_id, uint32_t chunk_idx,
        StreamingChunk &chunk, bool release_slot) {
    if (chunk.is_loaded) {
        // Loaded chunks should use unload/cancel paths, never rollback.
        ERR_FAIL_COND_MSG(chunk.upload_pending,
                vformat("[Streaming] Invalid rollback state: asset=%d chunk=%d is loaded and upload_pending.",
                        asset_id, chunk_idx));
        return;
    }

    const bool was_upload_pending = chunk.upload_pending;
    const uint32_t slot = chunk.buffer_slot;
    if (release_slot && slot != UINT32_MAX) {
        _release_chunk_slot_if_matches(atlas_allocator, _make_chunk_key(asset_id, chunk_idx), slot);
    }

    if (chunk.pending_upload_bytes > 0) {
        budget.pending_upload_bytes = budget.pending_upload_bytes > chunk.pending_upload_bytes
                ? (budget.pending_upload_bytes - chunk.pending_upload_bytes)
                : 0;
        chunk.pending_upload_bytes = 0;
    }
    if (was_upload_pending && budget.pending_upload_slots > 0) {
        budget.pending_upload_slots--;
    }
    chunk.upload_pending = false;
    chunk.gpu_resident = false;
    chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_FAILED_OR_ROLLED_BACK;
    chunk.upload_completion_mode = GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_NONE;
    chunk.upload_ticket_id = 0;
    chunk.upload_submit_frame = 0;
    chunk.upload_retire_frame = 0;
    chunk.upload_device = nullptr;
    chunk.buffer_slot = UINT32_MAX;
    global_atlas_registry.mark_chunk_meta_dirty(*this, asset_id, chunk_idx);
    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_rollback_pending_chunk",
            !release_slot);
}

void GaussianStreamingSystem::_process_upload_retirements() {
    if (pending_upload_retirements.is_empty()) {
        return;
    }

    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < pending_upload_retirements.size(); read_idx++) {
        PendingUploadRetirement ticket = pending_upload_retirements[read_idx];
        if (ticket.retire_frame > total_frame_count) {
            if (write_idx != read_idx) {
                pending_upload_retirements[write_idx] = ticket;
            }
            write_idx++;
            continue;
        }

        AtlasAssetState *asset = _get_asset_state(ticket.asset_id);
        if (!asset || asset->generation != ticket.asset_generation) {
            _release_cancelled_upload_retirement(atlas_allocator, budget, last_completed_upload_ticket_id,
                    ticket, _make_chunk_key(ticket.asset_id, ticket.chunk_idx));
            continue;
        }

        LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
        if (ticket.chunk_idx >= asset_chunks.size()) {
            _release_failed_upload_retirement(atlas_allocator, budget, last_completed_upload_ticket_id,
                    ticket, _make_chunk_key(ticket.asset_id, ticket.chunk_idx));
            continue;
        }

        StreamingChunk &chunk = asset_chunks[ticket.chunk_idx];
        if (!_retirement_ticket_matches_chunk(ticket, chunk)) {
            if (_retirement_state_mismatch_can_rollback(ticket, chunk)) {
                _mark_chunk_upload_failed(ticket.asset_id, ticket.chunk_idx, chunk,
                        "_process_upload_retirements.state_mismatch");
                _rollback_pending_chunk(ticket.asset_id, ticket.chunk_idx, chunk, true);
            } else {
                _release_chunk_slot_if_matches(atlas_allocator,
                        _make_chunk_key(ticket.asset_id, ticket.chunk_idx), ticket.buffer_slot);
            }
            last_completed_upload_ticket_id = ticket.ticket_id;
            continue;
        }

        _mark_upload_ticket_gpu_retired(chunk, ticket);
        _complete_chunk_load_common(ticket.asset_id, ticket.chunk_idx, chunk);
        _record_successful_upload_retirement(budget, total_sh_metrics,
                last_completed_upload_ticket_id, last_upload_completion_mode, ticket);
    }

    pending_upload_retirements.resize(write_idx);
}

Error GaussianStreamingSystem::_load_chunk(uint32_t asset_id, uint32_t chunk_idx) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset || (!asset->data.is_valid() && !asset->payload_source.is_valid())) {
        return ERR_DOES_NOT_EXIST;
    }

    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    if (chunk_idx >= asset_chunks.size()) {
        return ERR_INVALID_PARAMETER;
    }

    StreamingChunk &chunk = asset_chunks[chunk_idx];
    if (chunk.is_loaded || chunk.upload_pending) {
        return ERR_BUSY;
    }
    if (_has_pending_upload_retirement(asset_id, chunk_idx, UINT32_MAX)) {
        return ERR_BUSY;
    }

    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_load_chunk.pre");
    if (chunk.buffer_slot != UINT32_MAX) {
        _rollback_pending_chunk(asset_id, chunk_idx, chunk, true);
    }

    GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
    GaussianSplatManager::ScopedSubmissionLock submission_lock;
    RenderingDevice *submission_rd = _resolve_submission_device(manager, submission_lock);

    if (!submission_rd) {
        return ERR_UNAVAILABLE;
    }
    if (!persistent_buffer.is_valid()) {
        return ERR_UNAVAILABLE;
    }
    if (!atlas_allocator.has_free_slots()) {
        return ERR_BUSY;
    }

    uint32_t buffer_slot = UINT32_MAX;
    if (!atlas_allocator.allocate_slot(_make_chunk_key(asset_id, chunk_idx), buffer_slot)) {
        return ERR_BUSY;
    }

    if (!_begin_chunk_upload(asset_id, chunk_idx, chunk, buffer_slot)) {
        atlas_allocator.release_slot(_make_chunk_key(asset_id, chunk_idx));
        return FAILED;
    }

    const uint64_t buffer_offset64 = uint64_t(buffer_slot) * CHUNK_SIZE * sizeof(PackedGaussian);
    if (buffer_offset64 > uint64_t(UINT32_MAX)) {
        _rollback_pending_chunk(asset_id, chunk_idx, chunk, true);
        return FAILED;
    }
    const uint32_t buffer_offset = static_cast<uint32_t>(buffer_offset64);

    Vector<PackedGaussian> chunk_data;
    SHCompressionMetrics metrics;
    if (!_pack_chunk_data(asset_id, chunk_idx, *asset, chunk, chunk_data, metrics)) {
        _rollback_pending_chunk(asset_id, chunk_idx, chunk, true);
        return FAILED;
    }
    _log_chunk_load_metrics(chunk_idx, metrics);
    if (!_upload_chunk_to_gpu(submission_rd, buffer_offset, chunk_data, asset_id, chunk_idx, buffer_slot, chunk.count)) {
        _rollback_pending_chunk(asset_id, chunk_idx, chunk, true);
        return FAILED;
    }
    const uint64_t upload_bytes = uint64_t(chunk_data.size()) * sizeof(PackedGaussian);
    if (!_stage_chunk_upload_retirement(asset_id, chunk_idx, chunk, buffer_slot,
                upload_bytes, SHCompressionMetrics(), submission_rd)) {
        return FAILED;
    }
    _process_upload_retirements();
    return OK;
}

RenderingDevice *GaussianStreamingSystem::_resolve_submission_device(GaussianSplatManager *manager,
        GaussianSplatManager::ScopedSubmissionLock &submission_lock) const {
    RenderingDevice *rd = primary_device_override ? primary_device_override
            : (last_upload_device ? last_upload_device
                    : (manager ? manager->get_primary_rendering_device() : nullptr));
    if (!manager) {
        return rd;
    }
    RenderingDevice *submission_rd = manager->acquire_submission_device(rd, submission_lock);
    if (submission_rd) {
        return submission_rd;
    }
    // If the resolved device is not the shared submission device, uploads can
    // still proceed without the shared-device lock.
    if (rd && !manager->is_shared_submission_device(rd)) {
        return rd;
    }
    return nullptr;
}

bool GaussianStreamingSystem::_pack_chunk_data(uint32_t asset_id, uint32_t chunk_idx, const AtlasAssetState &asset, StreamingChunk &chunk,
        Vector<PackedGaussian> &chunk_data, SHCompressionMetrics &metrics) {
    chunk_data.clear();
    metrics = SHCompressionMetrics();

    // Resolve data source: prefer payload_source (supports out-of-core),
    // fall back to in-memory asset.data.
    const bool has_payload_source = asset.payload_source.is_valid();
    const bool has_data = asset.data.is_valid();
    if (!has_payload_source && !has_data) {
        return false;
    }

    // Lambdas that abstract contiguous / indexed reads regardless of source.
    const auto read_contiguous = [&](uint32_t start, uint32_t count,
            LocalVector<Gaussian> &g, LocalVector<Vector3> &sh,
            uint32_t &fo, uint32_t &ho) -> bool {
        if (has_payload_source) {
            return asset.payload_source->capture_chunk_snapshot(start, count, g, sh, fo, ho);
        }
        return asset.data->capture_chunk_snapshot(start, count, g, sh, fo, ho);
    };
    const auto read_indexed = [&](const uint32_t *indices, uint32_t count,
            LocalVector<Gaussian> &g, LocalVector<Vector3> &sh,
            uint32_t &fo, uint32_t &ho) -> bool {
        if (has_payload_source) {
            return asset.payload_source->capture_indexed_chunk_snapshot(indices, count, g, sh, fo, ho);
        }
        return asset.data->capture_indexed_chunk_snapshot(indices, count, g, sh, fo, ho);
    };

    LocalVector<Gaussian> gaussian_snapshot;
    LocalVector<Vector3> sh_high_order_snapshot;
    LocalVector<uint32_t> source_indices;
    uint32_t sh_first_order = 0;
    uint32_t sh_high_order = 0;
    if (chunk.source_index_remapped && asset_id == PRIMARY_ASSET_ID) {
        source_indices.resize(chunk.count);
        for (uint32_t i = 0; i < chunk.count; i++) {
            uint32_t source_index = 0;
            if (!_resolve_primary_chunk_source_index(chunk, i, source_index)) {
                return false;
            }
            source_indices[i] = source_index;
        }
        if (!read_indexed(source_indices.ptr(), chunk.count,
                    gaussian_snapshot, sh_high_order_snapshot, sh_first_order, sh_high_order)) {
            return false;
        }
    } else {
        if (!read_contiguous(chunk.start_idx, chunk.count,
                    gaussian_snapshot, sh_high_order_snapshot, sh_first_order, sh_high_order)) {
            return false;
        }
    }

    if (chunk.count == 0) {
        return true;
    }
    if (gaussian_snapshot.size() != chunk.count) {
        return false;
    }

    chunk_data.resize(chunk.count);

    static int gaussians_check_count = 0;
    if (++gaussians_check_count <= 3) {
        if (!gaussian_snapshot.is_empty()) {
            const Gaussian &g0 = gaussian_snapshot[0];
            GaussianSplatting::debug_trace_record_gaussians_check(asset_id,
                    gaussian_snapshot.size(),
                    chunk.start_idx,
                    chunk.count,
                    g0.sh_dc,
                    g0.opacity);
        } else {
            GaussianSplatting::debug_trace_record_gaussians_check(asset_id,
                    gaussian_snapshot.size(),
                    chunk.start_idx,
                    chunk.count,
                    Color(),
                    0.0f);
        }
    }

    static int diag_count = 0;
    if (++diag_count <= 3) {
        uint32_t zero_scale_count = 0;
        uint32_t zero_opacity_count = 0;
        uint32_t nan_pos_count = 0;
        for (uint32_t i = 0; i < chunk.count; i++) {
            const Gaussian &g = gaussian_snapshot[i];
            if (g.scale.x < 0.0001f && g.scale.y < 0.0001f && g.scale.z < 0.0001f) {
                zero_scale_count++;
            }
            if (g.opacity < 0.01f) {
                zero_opacity_count++;
            }
            if (!Math::is_finite(g.position.x) || !Math::is_finite(g.position.y) || !Math::is_finite(g.position.z)) {
                nan_pos_count++;
            }
        }
        GaussianSplatting::debug_trace_record_chunk_diagnostics(chunk_idx,
                zero_scale_count,
                zero_opacity_count,
                nan_pos_count,
                chunk.count);
    }

    const Vector3 *sh_coeffs = sh_high_order_snapshot.is_empty() ? nullptr : sh_high_order_snapshot.ptr();
    pack_gaussians_range(gaussian_snapshot,
            0,
            chunk.count,
            chunk_data,
            metrics,
            sh_coeffs,
            sh_first_order,
            sh_high_order);
    return true;
}

void GaussianStreamingSystem::_log_chunk_load_metrics(uint32_t chunk_idx, const SHCompressionMetrics &metrics) {
    if (metrics.coefficient_count > 0) {
        float raw_mb = metrics.raw_bytes / (1024.0f * 1024.0f);
        float compressed_mb = metrics.compressed_bytes / (1024.0f * 1024.0f);
        float ratio = metrics.raw_bytes > 0
                ? (metrics.compressed_bytes * 100.0f) / float(metrics.raw_bytes)
                : 0.0f;
        GS_LOG_STREAMING_DEBUG(vformat("[Streaming] Chunk %d SH compression: %.3f MB -> %.3f MB (%.1f%%)",
                chunk_idx, raw_mb, compressed_mb, ratio));
    }

    total_sh_metrics.raw_bytes += metrics.raw_bytes;
    total_sh_metrics.compressed_bytes += metrics.compressed_bytes;
    total_sh_metrics.coefficient_count += metrics.coefficient_count;

    if (total_sh_metrics.coefficient_count > 0) {
        float total_raw_mb = total_sh_metrics.raw_bytes / (1024.0f * 1024.0f);
        float total_compressed_mb = total_sh_metrics.compressed_bytes / (1024.0f * 1024.0f);
        float total_ratio = total_sh_metrics.raw_bytes > 0
                ? (total_sh_metrics.compressed_bytes * 100.0f) / float(total_sh_metrics.raw_bytes)
                : 0.0f;
        GS_LOG_STREAMING_DEBUG(vformat("[Streaming] Accumulated SH compression: %.3f MB -> %.3f MB (%.1f%%)",
                total_raw_mb, total_compressed_mb, total_ratio));
    }
}

bool GaussianStreamingSystem::_upload_chunk_to_gpu(RenderingDevice *submission_rd, uint32_t buffer_offset,
        const Vector<PackedGaussian> &chunk_data, uint32_t asset_id, uint32_t chunk_idx,
        uint32_t buffer_slot, uint32_t chunk_count) const {
#if defined(TESTS_ENABLED)
    if (const_cast<GaussianStreamingSystem *>(this)->test_force_next_chunk_upload_failure) {
        const_cast<GaussianStreamingSystem *>(this)->test_force_next_chunk_upload_failure = false;
        return false;
    }
#endif
    if (!submission_rd || !persistent_buffer.is_valid() || buffer_slot == UINT32_MAX || chunk_count == 0 || chunk_count > CHUNK_SIZE) {
        return false;
    }
    if (chunk_data.size() != static_cast<int>(chunk_count)) {
        return false;
    }
    const uint64_t upload_bytes = uint64_t(chunk_count) * sizeof(PackedGaussian);
    const uint64_t slot_capacity_bytes = uint64_t(CHUNK_SIZE) * sizeof(PackedGaussian);
    if (upload_bytes > slot_capacity_bytes) {
        return false;
    }
    if (upload_bytes > persistent_buffer_size || buffer_offset > persistent_buffer_size - upload_bytes) {
        return false;
    }

    submission_rd->buffer_update(persistent_buffer, buffer_offset,
            chunk_count * sizeof(PackedGaussian),
            chunk_data.ptr());
    if (submission_rd->is_main_rendering_device()) {
        gs_device_utils::safe_submit(submission_rd);
    } else {
        gs_device_utils::safe_submit_and_sync(submission_rd);
    }
    return true;
}

void GaussianStreamingSystem::_finalize_chunk_load(uint32_t asset_id, uint32_t chunk_idx,
        StreamingChunk &chunk, uint32_t buffer_slot, uint32_t asset_chunk_count) {
    if (!chunk.upload_pending || chunk.buffer_slot != buffer_slot) {
        _rollback_pending_chunk(asset_id, chunk_idx, chunk, true);
        return;
    }

    chunk.buffer_slot = buffer_slot;
    _complete_chunk_load_common(asset_id, chunk_idx, chunk);

    GS_LOG_STREAMING_DEBUG(vformat("[STREAM-LOAD] asset=%d chunk=%d/%d start=%d count=%d slot=%d loaded_total=%d",
            asset_id, chunk_idx, asset_chunk_count, chunk.start_idx, chunk.count,
            chunk.buffer_slot, budget.loaded_chunks_count));
}

void GaussianStreamingSystem::_complete_chunk_load_common(uint32_t asset_id, uint32_t chunk_idx, StreamingChunk &chunk) {
    if (chunk.pending_upload_bytes > 0) {
        budget.pending_upload_bytes = budget.pending_upload_bytes > chunk.pending_upload_bytes
                ? (budget.pending_upload_bytes - chunk.pending_upload_bytes)
                : 0;
        chunk.pending_upload_bytes = 0;
    }
    if (budget.pending_upload_slots > 0) {
        budget.pending_upload_slots--;
    }
    chunk.is_loaded = true;
    chunk.gpu_resident = true;
    chunk.upload_pending = false;
    chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_GPU_RETIRED;
    chunk.last_loaded_frame = total_frame_count;
    eviction_controller.touch_chunk_use(chunk.last_used_frame);
    budget.loaded_chunks_count++;
    budget.vram_usage += chunk.count * sizeof(PackedGaussian);
    eviction_controller.note_chunk_loaded(asset_id, chunk_idx);
    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (asset) {
        if (chunk.explicit_request_generation != 0) {
            _update_requested_chunk_state_for_generation(*asset, chunk_idx,
                    chunk.explicit_request_generation,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                    OK);
        } else {
            _update_requested_chunk_state(*asset, chunk_idx,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                    OK);
        }
    }
    chunk.explicit_request_generation = 0;
    chunk.upload_ticket_id = 0;
    chunk.upload_submit_frame = 0;
    chunk.upload_retire_frame = 0;
    chunk.upload_device = nullptr;
    global_atlas_registry.mark_chunk_meta_dirty(*this, asset_id, chunk_idx);
    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_complete_chunk_load_common");
}

void GaussianStreamingSystem::_unload_chunk(uint32_t chunk_idx) {
    _unload_chunk(PRIMARY_ASSET_ID, chunk_idx);
}

void GaussianStreamingSystem::_unload_chunk(uint32_t asset_id, uint32_t chunk_idx) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset) {
        return;
    }

    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    if (chunk_idx >= asset_chunks.size() || !asset_chunks[chunk_idx].is_loaded) {
        return;
    }

    StreamingChunk &chunk = asset_chunks[chunk_idx];
    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_unload_chunk.pre");
    if (chunk.buffer_slot != UINT32_MAX) {
        atlas_allocator.release_slot(_make_chunk_key(asset_id, chunk_idx));
    }

    chunk.is_loaded = false;
    chunk.gpu_resident = false;
    chunk.upload_pending = false;
    chunk.upload_lifecycle_state = GaussianStreamingTypes::STREAMING_UPLOAD_STATE_NONE;
    chunk.upload_completion_mode = GaussianStreamingTypes::STREAMING_UPLOAD_COMPLETION_NONE;
    chunk.upload_ticket_id = 0;
    chunk.upload_submit_frame = 0;
    chunk.upload_retire_frame = 0;
    chunk.pending_upload_bytes = 0;
    chunk.upload_device = nullptr;
    chunk.buffer_slot = UINT32_MAX; // Clear buffer slot
    eviction_controller.note_chunk_unloaded(asset_id, chunk_idx);
    global_atlas_registry.mark_chunk_meta_dirty(*this, asset_id, chunk_idx);
    _assert_chunk_state_invariant(asset_id, chunk_idx, chunk, "_unload_chunk.post");
    budget.loaded_chunks_count--;
    const uint64_t chunk_bytes = uint64_t(chunk.count) * sizeof(PackedGaussian);
    const uint64_t evicted_bytes = budget.vram_usage > chunk_bytes ? chunk_bytes : budget.vram_usage;
    budget.vram_usage = budget.vram_usage > chunk_bytes ? (budget.vram_usage - chunk_bytes) : 0;
    budget.evicted_bytes_total += evicted_bytes;
}

GaussianStreamingSystem::EvictionResult GaussianStreamingSystem::_evict_least_recently_used(bool p_allow_visible_eviction) {
    return eviction_controller.evict_least_recently_used(*this, p_allow_visible_eviction);
}

GaussianStreamingSystem::EvictionResult GaussianStreamingSystem::_evict_non_primary_lru() {
    return eviction_controller.evict_non_primary_lru(*this);
}

void GaussianStreamingSystem::begin_frame() {
    current_frame_idx = (current_frame_idx + 1) % RING_BUFFER_FRAMES;
    FrameData &frame = frame_data[current_frame_idx];
    frame.frame_number = total_frame_count++;
    frame.visible_chunks.clear();
    _reset_per_frame_counters();
    per_frame_counters_reset_for_streaming_update = true;
    _process_upload_retirements();
    if (memory_stream_proxy.is_valid()) {
        memory_stream_proxy->begin_frame(frame.frame_number);
    }
}

void GaussianStreamingSystem::end_frame() {
    if (memory_stream_proxy.is_valid()) {
        memory_stream_proxy->end_frame();
    }
    analytics_snapshot = memory_stream_proxy.is_valid() ? memory_stream_proxy->get_task_debug_state() : Dictionary();

    // Add VRAM usage stats to analytics. vram_mb mirrors the regulator-facing
    // total (full persistent buffer allocation + auxiliary overhead), so the
    // reported figure is not under-counted by the unfilled buffer remainder.
    const uint64_t payload_vram_bytes = budget.vram_usage;
    const uint64_t overhead_vram_bytes = _get_auxiliary_vram_overhead_bytes();
    // Mirror the live-RID gate in _get_total_vram_usage_bytes(): a failed
    // storage_buffer_create() leaves persistent_buffer_size nonzero with an invalid
    // RID, so report the allocation only when it actually exists. Otherwise vram_mb
    // (gated) and vram_persistent_buffer_mb (ungated) would disagree on the
    // allocation-failure path. (Codex #411)
    const uint64_t allocated_persistent_bytes = persistent_buffer.is_valid() ? uint64_t(persistent_buffer_size) : 0;
    analytics_snapshot["vram_mb"] = double(_get_total_vram_usage_bytes()) / (1024.0 * 1024.0);
    analytics_snapshot["vram_payload_mb"] = double(payload_vram_bytes) / (1024.0 * 1024.0);
    analytics_snapshot["vram_overhead_mb"] = double(overhead_vram_bytes) / (1024.0 * 1024.0);
    analytics_snapshot["vram_persistent_buffer_mb"] = double(allocated_persistent_bytes) / (1024.0 * 1024.0);
    analytics_snapshot["loaded_chunks"] = get_loaded_chunks();
    analytics_snapshot["atlas_published_chunks"] = global_atlas_registry.get_atlas_published_chunks();
    analytics_snapshot["visible_splats"] = get_visible_count();
    analytics_snapshot["effective_max_chunks"] = get_effective_max_chunks();
    analytics_snapshot["streaming_initial_capacity"] = static_cast<int64_t>(streaming_initial_capacity);
    analytics_snapshot["streaming_current_capacity"] = static_cast<int64_t>(streaming_current_capacity);
    analytics_snapshot["streaming_grow_count"] = static_cast<int64_t>(streaming_grow_count);
    analytics_snapshot["chunks_loaded_this_frame"] = budget.chunks_loaded_this_frame;
    analytics_snapshot["chunks_evicted_this_frame"] = eviction_controller.get_chunks_evicted_this_frame();
    analytics_snapshot["pending_upload_reserved_bytes"] = static_cast<int64_t>(_get_pending_upload_bytes_for_diagnostics());
    analytics_snapshot["pending_upload_reserved_slots"] = static_cast<int64_t>(get_pending_upload_retirement_slots());
    analytics_snapshot["pending_upload_retirement_tickets"] = static_cast<int64_t>(pending_upload_retirements.size());
    analytics_snapshot["retired_upload_bytes_this_frame"] = static_cast<int64_t>(budget.retired_upload_bytes_this_frame);
    analytics_snapshot["retired_upload_slots_this_frame"] = static_cast<int64_t>(budget.retired_upload_slots_this_frame);
    analytics_snapshot["failed_upload_retirements"] = static_cast<int64_t>(budget.failed_upload_retirements);
    analytics_snapshot["last_upload_completion_mode"] = last_upload_completion_mode;
    analytics_snapshot["zero_visible_consecutive_frames"] = visibility.zero_visible_recovery.zero_visible_consecutive_frames;
    analytics_snapshot["zero_visible_recoveries_triggered"] = (int)visibility.zero_visible_recovery.recoveries_triggered;
    analytics_snapshot["zero_visible_stall_detections"] = (int)visibility.zero_visible_recovery.stall_detections;
    analytics_snapshot["zero_visible_recovery_mode"] =
            visibility.zero_visible_recovery.mode == ZeroVisibleRecoveryMode::PERSISTENT ? "persistent" : "startup_only";
    analytics_snapshot["runtime_ready"] = is_runtime_ready();
    analytics_snapshot["runtime_capacity_zero"] = is_runtime_capacity_zero();
    analytics_snapshot["runtime_buffer_invalid"] = is_persistent_buffer_invalid();
    analytics_snapshot["invalid_camera_input_events"] = (int)invalid_camera_input_events;
    analytics_snapshot["cap_tier_preset"] = upload_pipeline.cap_tier_preset;
    analytics_snapshot["cap_tier_active"] = upload_pipeline.cap_tier_active;
    analytics_snapshot["effective_upload_cap_mb_per_frame"] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_frame);
    analytics_snapshot["effective_upload_cap_mb_per_slice"] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_slice);
    analytics_snapshot["effective_upload_cap_mb_per_second"] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_second);
    analytics_snapshot["cap_source_upload_mb_per_frame"] = upload_pipeline.cap_source_upload_mb_per_frame;
    analytics_snapshot["cap_source_upload_mb_per_slice"] = upload_pipeline.cap_source_upload_mb_per_slice;
    analytics_snapshot["cap_source_upload_mb_per_second"] = upload_pipeline.cap_source_upload_mb_per_second;
    analytics_snapshot["upload_frame_cap_hit"] = upload_pipeline.upload_frame_cap_hit_this_frame;
    analytics_snapshot["upload_slice_cap_hit"] = upload_pipeline.upload_slice_cap_hit_this_frame;
    analytics_snapshot["upload_bandwidth_cap_hit"] = upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    analytics_snapshot["chunk_load_cap_hit"] = upload_pipeline.chunk_load_cap_hit_this_frame;
    analytics_snapshot["vram_chunk_cap_hit"] = budget.vram_chunk_cap_hit_this_frame;

    // Add VRAM regulator stats if available
    if (budget.vram_regulator.is_valid()) {
        VRAMDebugStats vram_stats = budget.vram_regulator->get_debug_stats();
        const VRAMBudgetConfig &active_budget_config = budget.vram_regulator->get_config();
        analytics_snapshot["vram_budget_mb"] = float(vram_stats.budget_bytes) / (1024.0f * 1024.0f);
        analytics_snapshot["vram_usage_percent"] = vram_stats.usage_percent;
        analytics_snapshot["vram_budget_warning"] = vram_stats.budget_warning_active;
        analytics_snapshot["vram_regulation_adjustments"] = vram_stats.regulation_adjustments;
        analytics_snapshot["vram_thrashing_events"] = vram_stats.thrashing_events;
        analytics_snapshot["lod_distance_multiplier"] = budget.vram_regulator->get_lod_distance_multiplier();
        analytics_snapshot["effective_vram_budget_mb"] = static_cast<int64_t>(active_budget_config.budget_mb);
        analytics_snapshot["effective_vram_min_chunks"] = static_cast<int64_t>(active_budget_config.min_chunks);
        analytics_snapshot["effective_vram_max_chunks"] = static_cast<int64_t>(active_budget_config.max_chunks);
        analytics_snapshot["requested_vram_budget_mb"] = static_cast<int64_t>(active_budget_config.requested_budget_mb);
        analytics_snapshot["cap_source_vram_budget_mb"] = active_budget_config.source_budget_mb;
        analytics_snapshot["requested_cap_source_vram_budget_mb"] = active_budget_config.requested_source_budget_mb;
        analytics_snapshot["cap_source_vram_min_chunks"] = active_budget_config.source_min_chunks;
        analytics_snapshot["cap_source_vram_max_chunks"] = active_budget_config.source_max_chunks;
        analytics_snapshot["vram_budget_capacity_verified"] = active_budget_config.budget_capacity_verified;
        analytics_snapshot["vram_budget_unknown_capacity_fallback"] = active_budget_config.budget_uses_unknown_capacity_fallback;
        analytics_snapshot["vram_budget_unverified"] = active_budget_config.budget_unverified;
        analytics_snapshot["cap_tier_preset"] = active_budget_config.cap_tier_preset;
        analytics_snapshot["cap_tier_active"] = active_budget_config.cap_tier_active;
    }

    // Add distance-based LOD stats (Octree-GS)
    const LODConfig &lod_config = _get_lod_config();
    analytics_snapshot["lod_enabled"] = lod_config.enabled;
    analytics_snapshot["effective_splat_count"] = get_effective_splat_count();
    if (lod_config.enabled) {
        Dictionary lod_stats = get_lod_debug_stats();
        analytics_snapshot["lod_reduction_ratio"] = lod_stats.get("reduction_ratio", 0.0f);
    }

    // Sample pack/upload timing metrics (every frame for performance monitors).
    // PackTelemetry::exchange_and_reset() atomically drains all counters in one
    // cache-line-local sweep, replacing 15 scattered atomic exchanges.
    const StreamingUploadPipeline::PackTelemetry::Snapshot tsnap = upload_pipeline.telemetry.exchange_and_reset();

    upload_pipeline.last_pack_avg_ms = tsnap.pack_jobs > 0 ? double(tsnap.pack_time_total) / (1000.0 * double(tsnap.pack_jobs)) : 0.0;
    upload_pipeline.last_pack_max_ms = double(tsnap.pack_time_max) / 1000.0;
    upload_pipeline.last_pack_jobs = tsnap.pack_jobs;
    upload_pipeline.last_upload_mb = double(tsnap.upload_bytes) / (1024.0 * 1024.0);
    upload_pipeline.last_upload_chunks = tsnap.upload_chunks;
    upload_pipeline.last_pack_queue_latency_avg_ms = tsnap.pack_queue_lat_samples > 0
            ? double(tsnap.pack_queue_lat_total) / (USEC_PER_MS * double(tsnap.pack_queue_lat_samples))
            : 0.0;
    upload_pipeline.last_pack_queue_latency_max_ms = double(tsnap.pack_queue_lat_max) / USEC_PER_MS;
    upload_pipeline.last_upload_queue_latency_avg_ms = tsnap.upload_queue_lat_samples > 0
            ? double(tsnap.upload_queue_lat_total) / (USEC_PER_MS * double(tsnap.upload_queue_lat_samples))
            : 0.0;
    upload_pipeline.last_upload_queue_latency_max_ms = double(tsnap.upload_queue_lat_max) / USEC_PER_MS;
    upload_pipeline.last_pack_mutex_wait_avg_ms = tsnap.mutex_wait_samples > 0
            ? double(tsnap.mutex_wait_total) / (USEC_PER_MS * double(tsnap.mutex_wait_samples))
            : 0.0;
    upload_pipeline.last_pack_mutex_wait_max_ms = double(tsnap.mutex_wait_max) / USEC_PER_MS;
    upload_pipeline.last_thread_wakes = tsnap.thread_wakes;
    upload_pipeline.last_thread_dequeues = tsnap.thread_dequeues;
    upload_pipeline.last_thread_snapshots = tsnap.thread_snapshots;
    upload_pipeline.last_thread_enqueue_uploads = tsnap.thread_enqueue_uploads;

    analytics_snapshot["pack_avg_ms"] = upload_pipeline.last_pack_avg_ms;
    analytics_snapshot["pack_max_ms"] = upload_pipeline.last_pack_max_ms;
    analytics_snapshot["pack_jobs_completed"] = (int)upload_pipeline.last_pack_jobs;
    analytics_snapshot["upload_mb_this_frame"] = upload_pipeline.last_upload_mb;
    analytics_snapshot["upload_chunks_this_frame"] = (int)upload_pipeline.last_upload_chunks;
    analytics_snapshot["pack_queue_latency_avg_ms"] = upload_pipeline.last_pack_queue_latency_avg_ms;
    analytics_snapshot["pack_queue_latency_max_ms"] = upload_pipeline.last_pack_queue_latency_max_ms;
    analytics_snapshot["upload_queue_latency_avg_ms"] = upload_pipeline.last_upload_queue_latency_avg_ms;
    analytics_snapshot["upload_queue_latency_max_ms"] = upload_pipeline.last_upload_queue_latency_max_ms;
    analytics_snapshot["pack_mutex_wait_avg_ms"] = upload_pipeline.last_pack_mutex_wait_avg_ms;
    analytics_snapshot["pack_mutex_wait_max_ms"] = upload_pipeline.last_pack_mutex_wait_max_ms;
    analytics_snapshot["pack_thread_wakes"] = (int)upload_pipeline.last_thread_wakes;
    analytics_snapshot["pack_thread_dequeues"] = (int)upload_pipeline.last_thread_dequeues;
    analytics_snapshot["pack_thread_snapshots"] = (int)upload_pipeline.last_thread_snapshots;
    analytics_snapshot["pack_thread_enqueue_uploads"] = (int)upload_pipeline.last_thread_enqueue_uploads;
    analytics_snapshot["pack_jobs_in_flight"] = (int)upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed);
    analytics_snapshot["avg_chunk_load_time_ms"] = upload_pipeline.last_pack_avg_ms;
    analytics_snapshot["camera_velocity"] = visibility.camera_tracker.velocity.length();
    analytics_snapshot["prefetch_hits"] = (int)scheduler.last_prefetch_enqueued_count;
    const int64_t prefetch_miss_count =
            int64_t(scheduler.last_prefetch_candidate_count) - int64_t(scheduler.last_prefetch_enqueued_count);
    analytics_snapshot["prefetch_misses"] = (int)(prefetch_miss_count > 0 ? prefetch_miss_count : 0);
    analytics_snapshot["evicted_bytes_total_mb"] = double(budget.evicted_bytes_total) / (1024.0 * 1024.0);
    const float cull_effectiveness = visibility.culling_stats.total_chunks > 0
            ? float(visibility.culling_stats.frustum_culled_chunks) / float(visibility.culling_stats.total_chunks)
            : 0.0f;
    analytics_snapshot["cull_effectiveness"] = cull_effectiveness;
    analytics_snapshot["primary_spatial_chunking_enabled"] = primary_chunk_layout_metrics.spatial_partition_enabled;
    analytics_snapshot["primary_chunk_source_index_count"] = static_cast<int64_t>(primary_chunk_layout_metrics.source_index_count);
    analytics_snapshot["primary_chunk_avg_radius_ratio"] = primary_chunk_layout_metrics.avg_chunk_radius_ratio;
    analytics_snapshot["primary_chunk_max_radius_ratio"] = primary_chunk_layout_metrics.max_chunk_radius_ratio;
    analytics_snapshot["primary_chunk_bounds_volume_ratio"] = primary_chunk_layout_metrics.bounds_volume_ratio;
    const bool strict_layout_validation = _layout_hint_strict_validation_enabled();
    const Dictionary layout_hint_validation = _layout_hint_build_snapshot(this, strict_layout_validation);
    analytics_snapshot["layout_hint_validation"] = layout_hint_validation;
    analytics_snapshot["layout_hint_strict_mode_enabled"] = strict_layout_validation;
    analytics_snapshot["layout_hint_fallback_total"] = layout_hint_validation.get("fallback_total", int64_t(0));
    analytics_snapshot["layout_hint_fallback_io_total"] = layout_hint_validation.get("fallback_io_total", int64_t(0));
    analytics_snapshot["layout_hint_fallback_primary_total"] = layout_hint_validation.get("fallback_primary_total", int64_t(0));
    analytics_snapshot["layout_hint_strict_failure_total"] = layout_hint_validation.get("strict_failure_total", int64_t(0));
    analytics_snapshot["layout_hint_last_reason"] = layout_hint_validation.get("last_reason", String("none"));
    analytics_snapshot["layout_hint_last_reason_category"] = layout_hint_validation.get("last_reason_category", String("other"));
    analytics_snapshot["layout_hint_last_context"] = layout_hint_validation.get("last_context", String("none"));
    analytics_snapshot["layout_hint_last_usage"] = layout_hint_validation.get("last_usage", String("none"));

    uint32_t pack_queue_depth = 0;
    uint32_t upload_queue_depth = 0;
    const uint32_t sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_depth, upload_queue_depth);
    const uint32_t analytics_pack_jobs_in_flight = upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed);
    const bool pack_inflight_saturated = upload_pipeline.max_pack_jobs_in_flight > 0 &&
            analytics_pack_jobs_in_flight >= upload_pipeline.max_pack_jobs_in_flight;
    const bool sync_backpressure =
            scheduler.last_sync_fallback_dropped_count > 0 || scheduler.last_sync_fallback_stalled_count > 0;
    const bool analytics_visible_eviction_active = diagnostics.visible_evict_fallback_attempts > 0 &&
            diagnostics.visible_evict_fallback_successes < diagnostics.visible_evict_fallback_attempts;
    StreamingQueuePressureController::PressureSample queue_pressure_sample;
    queue_pressure_sample.pack_queue_depth = pack_queue_depth;
    queue_pressure_sample.upload_queue_depth = upload_queue_depth;
    queue_pressure_sample.sync_fallback_queue_depth = sync_fallback_queue_depth;
    queue_pressure_sample.pack_jobs_in_flight = analytics_pack_jobs_in_flight;
    queue_pressure_sample.pack_inflight_saturated = pack_inflight_saturated;
    queue_pressure_sample.upload_frame_cap_hit = upload_pipeline.upload_frame_cap_hit_this_frame;
    queue_pressure_sample.upload_bandwidth_cap_hit = upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    queue_pressure_sample.chunk_load_cap_hit = upload_pipeline.chunk_load_cap_hit_this_frame;
    queue_pressure_sample.vram_chunk_cap_hit = budget.vram_chunk_cap_hit_this_frame;
    queue_pressure_sample.sync_backpressure = sync_backpressure;
    queue_pressure_sample.visible_eviction_active = analytics_visible_eviction_active;
    const StreamingQueuePressureController::PressureSummary queue_pressure_summary =
            _summarize_queue_pressure_checked(queue_pressure_sample, "GaussianStreamingSystem::end_frame.analytics");
    StreamingQueuePressureController::latch_summary(queue_pressure_summary,
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason);
    _validate_queue_pressure_latched_state(
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason,
            "GaussianStreamingSystem::end_frame.analytics");
    analytics_snapshot["scheduler_pack_queue_depth"] = (int)pack_queue_depth;
    analytics_snapshot["scheduler_upload_queue_depth"] = (int)upload_queue_depth;
    analytics_snapshot["pending_uploads"] = (int)upload_queue_depth;
    analytics_snapshot["pending_upload_reserved_bytes"] = static_cast<int64_t>(_get_pending_upload_bytes_for_diagnostics());
    analytics_snapshot["pending_upload_reserved_slots"] = static_cast<int64_t>(get_pending_upload_retirement_slots());
    analytics_snapshot["pending_upload_retirement_tickets"] = static_cast<int64_t>(pending_upload_retirements.size());
    analytics_snapshot["scheduler_visible_scan_chunks"] = (int)scheduler.last_visible_scan_count;
    analytics_snapshot["scheduler_load_candidates"] = (int)scheduler.last_load_candidate_count;
    analytics_snapshot["scheduler_primary_eviction_scan_chunks"] = (int)scheduler.last_primary_eviction_scan_count;
    analytics_snapshot["scheduler_primary_eviction_candidates"] = (int)scheduler.last_primary_eviction_candidate_count;
    analytics_snapshot["scheduler_non_primary_scan_chunks"] = (int)scheduler.last_non_primary_scan_count;
    analytics_snapshot["scheduler_non_primary_eviction_candidates"] = (int)scheduler.last_non_primary_eviction_candidate_count;
    analytics_snapshot["scheduler_prefetch_scan_chunks"] = (int)scheduler.last_prefetch_scan_count;
    analytics_snapshot["scheduler_prefetch_candidates"] = (int)scheduler.last_prefetch_candidate_count;
    analytics_snapshot["scheduler_prefetch_upload_pending_skips"] =
            static_cast<int64_t>(scheduler.last_prefetch_upload_pending_skip_count);
    analytics_snapshot["scheduler_prefetch_enqueue_headroom_stalls"] =
            static_cast<int64_t>(scheduler.last_prefetch_enqueue_headroom_stall_count);
    scheduler.last_cpu_total_attributed_ms =
            scheduler.last_visibility_cpu_ms +
            scheduler.last_load_cpu_ms +
            scheduler.last_build_visible_cpu_ms +
            scheduler.last_prefetch_cpu_ms;
    scheduler.last_cpu_unattributed_ms = MAX(
            0.0, scheduler.last_update_cpu_ms - scheduler.last_cpu_total_attributed_ms);
    analytics_snapshot["scheduler_update_cpu_ms"] = scheduler.last_update_cpu_ms;
    analytics_snapshot["scheduler_visibility_cpu_ms"] = scheduler.last_visibility_cpu_ms;
    analytics_snapshot["scheduler_load_cpu_ms"] = scheduler.last_load_cpu_ms;
    analytics_snapshot["scheduler_build_visible_cpu_ms"] = scheduler.last_build_visible_cpu_ms;
    analytics_snapshot["scheduler_prefetch_cpu_ms"] = scheduler.last_prefetch_cpu_ms;
    analytics_snapshot["scheduler_cpu_total_attributed_ms"] = scheduler.last_cpu_total_attributed_ms;
    analytics_snapshot["scheduler_cpu_unattributed_ms"] = scheduler.last_cpu_unattributed_ms;
    analytics_snapshot["scheduler_visible_scan_budget_effective"] =
            static_cast<int64_t>(scheduler.last_visible_scan_budget_effective);
    analytics_snapshot["scheduler_prefetch_scan_budget_effective"] =
            static_cast<int64_t>(scheduler.last_prefetch_scan_budget_effective);
    analytics_snapshot["scheduler_queue_pressure_scan_throttle_active"] =
            scheduler.queue_pressure_candidate_scan_throttle_active;
    analytics_snapshot["scheduler_queue_pressure_scan_throttle_queue_depth"] =
            static_cast<int64_t>(scheduler.queue_pressure_candidate_scan_throttle_queue_depth);
    analytics_snapshot["scheduler_queue_pressure_scan_throttle_enabled"] =
            scheduler.queue_pressure_candidate_scan_throttle_enabled;
    analytics_snapshot["scheduler_force_sync_fallback_due_to_async_stall"] =
            scheduler.force_sync_fallback_due_to_async_stall;
    analytics_snapshot["scheduler_sync_fallback_queue_depth"] = (int)sync_fallback_queue_depth;
    analytics_snapshot["scheduler_sync_fallback_enqueued"] = (int)scheduler.last_sync_fallback_enqueued_count;
    analytics_snapshot["scheduler_sync_fallback_drained"] = (int)scheduler.last_sync_fallback_drained_count;
    analytics_snapshot["scheduler_sync_fallback_dropped"] = (int)scheduler.last_sync_fallback_dropped_count;
    analytics_snapshot["scheduler_sync_fallback_stalled"] = (int)scheduler.last_sync_fallback_stalled_count;
    analytics_snapshot["scheduler_sync_fallback_load_budget"] = (int)scheduler.max_sync_fallback_loads_per_frame;
    analytics_snapshot["scheduler_sync_fallback_cpu_ms"] = scheduler.last_sync_fallback_cpu_ms;
    analytics_snapshot["sync_promoted_pack_jobs_this_frame"] = (int)upload_pipeline.last_sync_promoted_pack_jobs;
    analytics_snapshot["sync_promoted_pack_jobs_total"] = static_cast<int64_t>(upload_pipeline.sync_promoted_pack_jobs_total);
    StreamingTelemetryAdapter::QueuePressureSnapshot queue_pressure_snapshot;
    queue_pressure_snapshot.active = upload_pipeline.queue_pressure_active;
    queue_pressure_snapshot.source = upload_pipeline.queue_pressure_source;
    queue_pressure_snapshot.reason = upload_pipeline.queue_pressure_reason;
    queue_pressure_snapshot.backlog_depth = queue_pressure_summary.backlog_depth;
    queue_pressure_snapshot.total_pending_chunks = queue_pressure_summary.total_pending_chunks;
    queue_pressure_snapshot.pack_source_active = queue_pressure_summary.pack_source_active;
    queue_pressure_snapshot.upload_source_active = queue_pressure_summary.upload_source_active;
    queue_pressure_snapshot.sync_source_active = queue_pressure_summary.sync_source_active;
    StreamingTelemetryAdapter::apply_queue_pressure_analytics(analytics_snapshot, queue_pressure_snapshot);

    Dictionary cap_sources;
    cap_sources["max_upload_mb_per_frame"] = upload_pipeline.cap_source_upload_mb_per_frame;
    cap_sources["max_upload_mb_per_slice"] = upload_pipeline.cap_source_upload_mb_per_slice;
    cap_sources["max_upload_mb_per_second"] = upload_pipeline.cap_source_upload_mb_per_second;
    cap_sources["vram_budget_mb"] = analytics_snapshot.get("cap_source_vram_budget_mb", String("project_default"));
    cap_sources["min_chunks_in_vram"] = analytics_snapshot.get("cap_source_vram_min_chunks", String("project_default"));
    cap_sources["max_chunks_in_vram"] = analytics_snapshot.get("cap_source_vram_max_chunks", String("project_default"));
    analytics_snapshot["cap_sources"] = cap_sources;
    analytics_snapshot["visible_evict_fallback_attempts"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_attempts);
    analytics_snapshot["visible_evict_fallback_successes"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_successes);

    Dictionary streaming_diagnostics = _build_streaming_diagnostics_snapshot(
            pack_queue_depth, upload_queue_depth, sync_fallback_queue_depth);
    analytics_snapshot["diagnostics"] = streaming_diagnostics;
    analytics_snapshot["diagnostics_category"] = streaming_diagnostics.get("category", "ok");
    analytics_snapshot["diagnostics_reason"] = streaming_diagnostics.get("reason", "healthy");
    analytics_snapshot["diagnostics_fingerprint"] = streaming_diagnostics.get("fingerprint", "ok");
    analytics_snapshot["diagnostics_has_failure"] = streaming_diagnostics.get("has_failure", false);
}

Dictionary GaussianStreamingSystem::_build_streaming_diagnostics_snapshot(
        uint32_t pack_queue_depth, uint32_t upload_queue_depth, uint32_t sync_fallback_queue_depth) {
    Dictionary diagnostics_snapshot;

    String runtime_reason;
    const bool runtime_ready = is_runtime_ready(&runtime_reason);
    const uint32_t total_chunks = visibility.culling_stats.total_chunks;
    const uint32_t visible_chunks = visibility.culling_stats.visible_chunks;
    const uint32_t loaded_chunks = budget.loaded_chunks_count;
    const uint32_t reserved_upload_slots = get_pending_upload_retirement_slots();
    const uint64_t reserved_upload_bytes = _get_pending_upload_bytes_for_diagnostics();
    const uint32_t load_candidates = scheduler.last_load_candidate_count;
    const uint32_t loaded_this_frame = budget.chunks_loaded_this_frame;
    const uint32_t upload_chunks_this_frame = upload_pipeline.last_upload_chunks;
    const uint32_t visible_splats = get_visible_count();
    const float cull_effectiveness = total_chunks > 0
            ? float(visibility.culling_stats.frustum_culled_chunks) / float(total_chunks)
            : 0.0f;
    const bool upload_frame_cap_hit = upload_pipeline.upload_frame_cap_hit_this_frame;
    const bool upload_slice_cap_hit = upload_pipeline.upload_slice_cap_hit_this_frame;
    const bool upload_bandwidth_cap_hit = upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    const bool chunk_load_cap_hit = upload_pipeline.chunk_load_cap_hit_this_frame;
    const bool vram_chunk_cap_hit = budget.vram_chunk_cap_hit_this_frame;
    const uint32_t current_pack_jobs_in_flight = upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed);
    const bool pack_inflight_saturated = upload_pipeline.max_pack_jobs_in_flight > 0 &&
            current_pack_jobs_in_flight >= upload_pipeline.max_pack_jobs_in_flight;
    const bool sync_backpressure =
            scheduler.last_sync_fallback_dropped_count > 0 || scheduler.last_sync_fallback_stalled_count > 0;
    const bool visible_eviction_active = diagnostics.visible_evict_fallback_attempts > 0 &&
            diagnostics.visible_evict_fallback_successes < diagnostics.visible_evict_fallback_attempts;
    StreamingQueuePressureController::PressureSample queue_pressure_sample;
    queue_pressure_sample.pack_queue_depth = pack_queue_depth;
    queue_pressure_sample.upload_queue_depth = upload_queue_depth;
    queue_pressure_sample.sync_fallback_queue_depth = sync_fallback_queue_depth;
    queue_pressure_sample.pack_jobs_in_flight = current_pack_jobs_in_flight;
    queue_pressure_sample.pack_inflight_saturated = pack_inflight_saturated;
    queue_pressure_sample.upload_frame_cap_hit = upload_frame_cap_hit;
    queue_pressure_sample.upload_bandwidth_cap_hit = upload_bandwidth_cap_hit;
    queue_pressure_sample.chunk_load_cap_hit = chunk_load_cap_hit;
    queue_pressure_sample.vram_chunk_cap_hit = vram_chunk_cap_hit;
    queue_pressure_sample.sync_backpressure = sync_backpressure;
    queue_pressure_sample.visible_eviction_active = visible_eviction_active;
    const StreamingQueuePressureController::PressureSummary queue_pressure_summary =
            _summarize_queue_pressure_checked(queue_pressure_sample,
                    "GaussianStreamingSystem::_build_streaming_diagnostics_snapshot");
    StreamingQueuePressureController::latch_summary(queue_pressure_summary,
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason);
    _validate_queue_pressure_latched_state(
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason,
            "GaussianStreamingSystem::_build_streaming_diagnostics_snapshot");
    const bool queue_pressure_active = upload_pipeline.queue_pressure_active;
    const String queue_pressure_source = upload_pipeline.queue_pressure_source;
    const String queue_pressure_reason = upload_pipeline.queue_pressure_reason;
    VRAMBudgetConfig active_vram_caps;
    if (budget.vram_regulator.is_valid()) {
        active_vram_caps = budget.vram_regulator->get_config();
    }
    const int64_t effective_vram_budget_mb = budget.vram_regulator.is_valid() ? static_cast<int64_t>(active_vram_caps.budget_mb) : 0;
    const int64_t effective_vram_min_chunks = budget.vram_regulator.is_valid() ? static_cast<int64_t>(active_vram_caps.min_chunks) : 0;
    const int64_t effective_vram_max_chunks = budget.vram_regulator.is_valid() ? static_cast<int64_t>(active_vram_caps.max_chunks) : 0;

    if (!runtime_ready) {
        diagnostics.init_invalid_frames++;
    } else {
        diagnostics.init_invalid_frames = 0;
    }

    if (runtime_ready && total_chunks > 0 && visible_chunks == 0) {
        diagnostics.culling_empty_frames++;
    } else {
        diagnostics.culling_empty_frames = 0;
    }

    const bool loaded_progress = loaded_this_frame > 0 || loaded_chunks > diagnostics.last_loaded_chunks;
    const bool scheduler_stalled = runtime_ready &&
            total_chunks > 0 &&
            visible_chunks > 0 &&
            load_candidates > 0 &&
            !loaded_progress &&
            pack_queue_depth == 0 &&
            upload_queue_depth == 0 &&
            sync_fallback_queue_depth == 0;
    if (scheduler_stalled) {
        diagnostics.scheduler_stall_frames++;
    } else {
        diagnostics.scheduler_stall_frames = 0;
    }

    const bool upload_stalled = runtime_ready &&
            (pack_queue_depth > 0 || upload_queue_depth > 0 || reserved_upload_slots > 0) &&
            upload_chunks_this_frame == 0 &&
            loaded_this_frame == 0;
    if (upload_stalled) {
        diagnostics.upload_stall_frames++;
    } else {
        diagnostics.upload_stall_frames = 0;
    }

    const bool sync_fallback_stalled = runtime_ready &&
            !upload_pipeline.pack_thread_running.load() &&
            sync_fallback_queue_depth > 0 &&
            loaded_this_frame == 0;
    if (sync_fallback_stalled) {
        diagnostics.sync_fallback_stall_frames++;
    } else {
        diagnostics.sync_fallback_stall_frames = 0;
    }
    if (queue_pressure_active) {
        diagnostics.queue_pressure_frames++;
    } else {
        diagnostics.queue_pressure_frames = 0;
    }
    if (vram_chunk_cap_hit) {
        diagnostics.vram_cap_hit_frames++;
    } else {
        diagnostics.vram_cap_hit_frames = 0;
    }

    // Sustained pressure categorization: classify the dominant bottleneck
    // so long-soak diagnostics can distinguish bandwidth limits, residency
    // limits, churn, and pipeline stalls.
    {
        const uint32_t chunks_evicted = eviction_controller.get_chunks_evicted_this_frame();
        const uint32_t visible_evicted = eviction_controller.get_visible_chunks_evicted_this_frame();
        const bool bandwidth_signal = upload_frame_cap_hit || upload_bandwidth_cap_hit || chunk_load_cap_hit;
        const bool residency_signal = vram_chunk_cap_hit && chunks_evicted > 0;
        const bool churn_signal = visible_evicted > 0 && loaded_this_frame > 0;
        const bool stall_signal = upload_stalled || sync_fallback_stalled || scheduler_stalled;

        const uint32_t active_signals = uint32_t(bandwidth_signal) + uint32_t(residency_signal) +
                uint32_t(churn_signal) + uint32_t(stall_signal);

        if (active_signals > 1) {
            diagnostics.pressure_category = "combined";
            diagnostics.sustained_pressure_frames++;
        } else if (churn_signal) {
            diagnostics.pressure_category = "churn";
            diagnostics.sustained_pressure_frames++;
        } else if (residency_signal) {
            diagnostics.pressure_category = "residency_limited";
            diagnostics.sustained_pressure_frames++;
        } else if (bandwidth_signal) {
            diagnostics.pressure_category = "bandwidth_limited";
            diagnostics.sustained_pressure_frames++;
        } else if (stall_signal) {
            diagnostics.pressure_category = "pipeline_stall";
            diagnostics.sustained_pressure_frames++;
        } else {
            if (diagnostics.sustained_pressure_frames > 0) {
                if (diagnostics.sustained_pressure_frames <= DiagnosticsState::SUSTAINED_PRESSURE_COOLDOWN_FRAMES) {
                    diagnostics.sustained_pressure_frames = 0;
                    diagnostics.pressure_category = "none";
                } else {
                    diagnostics.sustained_pressure_frames--;
                }
            } else {
                diagnostics.pressure_category = "none";
            }
        }
    }

    String category = "ok";
    String reason = "healthy";
    if (!runtime_ready) {
        category = "init_invalid";
        reason = runtime_reason;
    } else if (budget.failed_upload_retirements > 0) {
        category = "upload_retirement_failed";
        reason = diagnostics.last_invariant_message.is_empty() ?
                vformat("upload retirements failed=%d.", static_cast<int64_t>(budget.failed_upload_retirements)) :
                diagnostics.last_invariant_message;
    } else if (diagnostics.integrity_mismatch_count > 0) {
        category = "integrity_mismatch";
        reason = diagnostics.last_integrity_mismatch_message.is_empty() ?
                vformat("integrity mismatches detected=%d.", static_cast<int64_t>(diagnostics.integrity_mismatch_count)) :
                diagnostics.last_integrity_mismatch_message;
    } else if (diagnostics.culling_empty_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "culling_empty";
        reason = vformat("visible_chunks=0 for %d frames while total_chunks=%d.",
                diagnostics.culling_empty_frames, total_chunks);
    } else if (diagnostics.upload_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "upload_stalled";
        reason = vformat("upload queue stalled for %d frames (pack_queue=%d upload_queue=%d pending_retire=%d).",
                diagnostics.upload_stall_frames, pack_queue_depth, upload_queue_depth, reserved_upload_slots);
    } else if (diagnostics.sync_fallback_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "sync_fallback_stalled";
        reason = vformat("sync fallback queue stalled for %d frames (queue=%d).",
                diagnostics.sync_fallback_stall_frames, sync_fallback_queue_depth);
    } else if (diagnostics.queue_pressure_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "queue_pressure";
        reason = vformat("queue pressure active for %d frames (source=%s reason=%s pack=%d upload=%d sync=%d).",
                diagnostics.queue_pressure_frames,
                queue_pressure_source,
                queue_pressure_reason,
                pack_queue_depth,
                upload_queue_depth,
                sync_fallback_queue_depth);
    } else if (diagnostics.vram_cap_hit_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "vram_cap_hit";
        reason = vformat("VRAM chunk cap blocked loads for %d frames (loaded=%d max=%d).",
                diagnostics.vram_cap_hit_frames, loaded_chunks, effective_vram_max_chunks);
    } else if (diagnostics.scheduler_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "scheduler_stalled";
        reason = vformat("load candidates present but no chunk progress for %d frames.",
                diagnostics.scheduler_stall_frames);
    }

    const bool has_failure = category != "ok";
    const String fingerprint = vformat(
            "%s|ready=%s|chunks=%d/%d/%d|visible_splats=%d|cand=%d|pack_q=%d|upload_q=%d|retire_q=%d|sync_q=%d|pack_flight=%d|total_pend=%d|load_frame=%d|upload_frame=%d|caps=%d/%d/%d|vram=%d/%d|hits=%d%d%d%d%d%d|qsrc=%s|qwhy=%s|atlas=%d|req=%d|inv=%d/%d/%d|sync_promo=%d",
            category,
            runtime_ready ? "1" : "0",
            loaded_chunks,
            visible_chunks,
            total_chunks,
            visible_splats,
            load_candidates,
            pack_queue_depth,
            upload_queue_depth,
            reserved_upload_slots,
            sync_fallback_queue_depth,
            current_pack_jobs_in_flight,
            queue_pressure_summary.total_pending_chunks,
            loaded_this_frame,
            upload_chunks_this_frame,
            upload_pipeline.effective_upload_cap_mb_per_frame,
            upload_pipeline.effective_upload_cap_mb_per_slice,
            upload_pipeline.effective_upload_cap_mb_per_second,
            effective_vram_budget_mb,
            effective_vram_max_chunks,
            upload_frame_cap_hit ? 1 : 0,
            upload_bandwidth_cap_hit ? 1 : 0,
            chunk_load_cap_hit ? 1 : 0,
            vram_chunk_cap_hit ? 1 : 0,
            queue_pressure_active ? 1 : 0,
            visible_eviction_active ? 1 : 0,
            queue_pressure_source,
            queue_pressure_reason,
            static_cast<int64_t>(global_atlas_registry.get_atlas_generation()),
            static_cast<int64_t>(asset_registry.request_generation),
            static_cast<int64_t>(diagnostics.invariant_slot_ownership_violations),
            static_cast<int64_t>(diagnostics.invariant_upload_lifecycle_violations),
            static_cast<int64_t>(diagnostics.invariant_generation_violations),
            static_cast<int64_t>(upload_pipeline.sync_promoted_pack_jobs_total));

    if (has_failure) {
        const bool fingerprint_changed = diagnostics.last_logged_fingerprint != fingerprint;
        const bool log_due = diagnostics.last_fingerprint_log_frame == 0 ||
                (total_frame_count - diagnostics.last_fingerprint_log_frame) >= DiagnosticsState::LOG_INTERVAL_FRAMES;
        if (fingerprint_changed || log_due) {
            WARN_PRINT(vformat("[Streaming][Diag:%s] fingerprint=%s reason=%s",
                    category, fingerprint, reason));
            diagnostics.last_logged_fingerprint = fingerprint;
            diagnostics.last_fingerprint_log_frame = total_frame_count;
        }
    } else if (diagnostics.active_category != "ok") {
        WARN_PRINT(vformat("[Streaming][Diag:ok] recovered_from=%s", diagnostics.active_category));
    }

    diagnostics.active_category = category;
    diagnostics.active_reason = reason;
    diagnostics.active_fingerprint = fingerprint;
    diagnostics.last_total_chunks = total_chunks;
    diagnostics.last_visible_chunks = visible_chunks;
    diagnostics.last_loaded_chunks = loaded_chunks;

    diagnostics_snapshot["category"] = category;
    diagnostics_snapshot["reason"] = reason;
    diagnostics_snapshot["fingerprint"] = fingerprint;
    diagnostics_snapshot["has_failure"] = has_failure;
    diagnostics_snapshot["runtime_ready"] = runtime_ready;
    diagnostics_snapshot["total_chunks"] = static_cast<int64_t>(total_chunks);
    diagnostics_snapshot["visible_chunks"] = static_cast<int64_t>(visible_chunks);
    diagnostics_snapshot["loaded_chunks"] = static_cast<int64_t>(loaded_chunks);
    diagnostics_snapshot["visible_splats"] = static_cast<int64_t>(visible_splats);
    diagnostics_snapshot["cull_effectiveness"] = cull_effectiveness;
    diagnostics_snapshot["primary_spatial_chunking_enabled"] = primary_chunk_layout_metrics.spatial_partition_enabled;
    diagnostics_snapshot["primary_chunk_source_index_count"] = static_cast<int64_t>(primary_chunk_layout_metrics.source_index_count);
    diagnostics_snapshot["primary_chunk_avg_radius_ratio"] = primary_chunk_layout_metrics.avg_chunk_radius_ratio;
    diagnostics_snapshot["primary_chunk_max_radius_ratio"] = primary_chunk_layout_metrics.max_chunk_radius_ratio;
    diagnostics_snapshot["primary_chunk_bounds_volume_ratio"] = primary_chunk_layout_metrics.bounds_volume_ratio;
    const bool strict_layout_validation = _layout_hint_strict_validation_enabled();
    const Dictionary layout_hint_validation = _layout_hint_build_snapshot(this, strict_layout_validation);
    diagnostics_snapshot["layout_hint_validation"] = layout_hint_validation;
    diagnostics_snapshot["layout_hint_strict_mode_enabled"] = strict_layout_validation;
    diagnostics_snapshot["layout_hint_fallback_total"] = layout_hint_validation.get("fallback_total", int64_t(0));
    diagnostics_snapshot["layout_hint_fallback_io_total"] = layout_hint_validation.get("fallback_io_total", int64_t(0));
    diagnostics_snapshot["layout_hint_fallback_primary_total"] = layout_hint_validation.get("fallback_primary_total", int64_t(0));
    diagnostics_snapshot["layout_hint_strict_failure_total"] = layout_hint_validation.get("strict_failure_total", int64_t(0));
    diagnostics_snapshot["layout_hint_last_reason"] = layout_hint_validation.get("last_reason", String("none"));
    diagnostics_snapshot["layout_hint_last_reason_category"] = layout_hint_validation.get("last_reason_category", String("other"));
    diagnostics_snapshot["layout_hint_last_context"] = layout_hint_validation.get("last_context", String("none"));
    diagnostics_snapshot["layout_hint_last_usage"] = layout_hint_validation.get("last_usage", String("none"));
    diagnostics_snapshot["load_candidates"] = static_cast<int64_t>(load_candidates);
    diagnostics_snapshot["scheduler_primary_eviction_scan_chunks"] =
            static_cast<int64_t>(scheduler.last_primary_eviction_scan_count);
    diagnostics_snapshot["scheduler_primary_eviction_candidates"] =
            static_cast<int64_t>(scheduler.last_primary_eviction_candidate_count);
    diagnostics_snapshot["scheduler_non_primary_scan_chunks"] =
            static_cast<int64_t>(scheduler.last_non_primary_scan_count);
    diagnostics_snapshot["scheduler_non_primary_eviction_candidates"] =
            static_cast<int64_t>(scheduler.last_non_primary_eviction_candidate_count);
    diagnostics_snapshot["pack_queue_depth"] = static_cast<int64_t>(pack_queue_depth);
    diagnostics_snapshot["upload_queue_depth"] = static_cast<int64_t>(upload_queue_depth);
    diagnostics_snapshot["pending_upload_reserved_slots"] = static_cast<int64_t>(reserved_upload_slots);
    diagnostics_snapshot["pending_upload_reserved_bytes"] = static_cast<int64_t>(reserved_upload_bytes);
    diagnostics_snapshot["pending_upload_retirement_tickets"] = static_cast<int64_t>(pending_upload_retirements.size());
    diagnostics_snapshot["sync_fallback_queue_depth"] = static_cast<int64_t>(sync_fallback_queue_depth);
    diagnostics_snapshot["pack_jobs_in_flight"] = static_cast<int64_t>(current_pack_jobs_in_flight);
    diagnostics_snapshot["total_pending_chunks"] = static_cast<int64_t>(queue_pressure_summary.total_pending_chunks);
    diagnostics_snapshot["visible_eviction_active"] = visible_eviction_active;
    StreamingTelemetryAdapter::QueuePressureSnapshot queue_pressure_snapshot;
    queue_pressure_snapshot.active = queue_pressure_active;
    queue_pressure_snapshot.source = queue_pressure_source;
    queue_pressure_snapshot.reason = queue_pressure_reason;
    queue_pressure_snapshot.backlog_depth = queue_pressure_summary.backlog_depth;
    queue_pressure_snapshot.total_pending_chunks = queue_pressure_summary.total_pending_chunks;
    queue_pressure_snapshot.pack_source_active = queue_pressure_summary.pack_source_active;
    queue_pressure_snapshot.upload_source_active = queue_pressure_summary.upload_source_active;
    queue_pressure_snapshot.sync_source_active = queue_pressure_summary.sync_source_active;
    StreamingTelemetryAdapter::apply_queue_pressure_diagnostics(diagnostics_snapshot, queue_pressure_snapshot);
    diagnostics_snapshot["scheduler_visible_scan_budget_effective"] =
            static_cast<int64_t>(scheduler.last_visible_scan_budget_effective);
    diagnostics_snapshot["scheduler_prefetch_scan_budget_effective"] =
            static_cast<int64_t>(scheduler.last_prefetch_scan_budget_effective);
    diagnostics_snapshot["scheduler_prefetch_upload_pending_skips"] =
            static_cast<int64_t>(scheduler.last_prefetch_upload_pending_skip_count);
    diagnostics_snapshot["scheduler_prefetch_enqueue_headroom_stalls"] =
            static_cast<int64_t>(scheduler.last_prefetch_enqueue_headroom_stall_count);
    diagnostics_snapshot["scheduler_queue_pressure_scan_throttle_active"] =
            scheduler.queue_pressure_candidate_scan_throttle_active;
    diagnostics_snapshot["scheduler_queue_pressure_scan_throttle_queue_depth"] =
            static_cast<int64_t>(scheduler.queue_pressure_candidate_scan_throttle_queue_depth);
    diagnostics_snapshot["scheduler_queue_pressure_scan_throttle_enabled"] =
            scheduler.queue_pressure_candidate_scan_throttle_enabled;
    diagnostics_snapshot["scheduler_force_sync_fallback_due_to_async_stall"] =
            scheduler.force_sync_fallback_due_to_async_stall;
    diagnostics_snapshot["chunks_loaded_this_frame"] = static_cast<int64_t>(loaded_this_frame);
    diagnostics_snapshot["chunks_uploaded_this_frame"] = static_cast<int64_t>(upload_chunks_this_frame);
    diagnostics_snapshot["retired_upload_bytes_this_frame"] = static_cast<int64_t>(budget.retired_upload_bytes_this_frame);
    diagnostics_snapshot["retired_upload_slots_this_frame"] = static_cast<int64_t>(budget.retired_upload_slots_this_frame);
    diagnostics_snapshot["failed_upload_retirements"] = static_cast<int64_t>(budget.failed_upload_retirements);
    diagnostics_snapshot["last_upload_completion_mode"] = last_upload_completion_mode;
    diagnostics_snapshot["last_completed_upload_ticket_id"] = static_cast<int64_t>(last_completed_upload_ticket_id);
    diagnostics_snapshot["sync_promoted_pack_jobs_this_frame"] = static_cast<int64_t>(upload_pipeline.last_sync_promoted_pack_jobs);
    diagnostics_snapshot["sync_promoted_pack_jobs_total"] = static_cast<int64_t>(upload_pipeline.sync_promoted_pack_jobs_total);
    diagnostics_snapshot["pack_thread_wakes"] = static_cast<int64_t>(upload_pipeline.last_thread_wakes);
    diagnostics_snapshot["pack_thread_dequeues"] = static_cast<int64_t>(upload_pipeline.last_thread_dequeues);
    diagnostics_snapshot["pack_thread_snapshots"] = static_cast<int64_t>(upload_pipeline.last_thread_snapshots);
    diagnostics_snapshot["pack_thread_enqueue_uploads"] = static_cast<int64_t>(upload_pipeline.last_thread_enqueue_uploads);
    diagnostics_snapshot["cap_tier_preset"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.cap_tier_preset
            : upload_pipeline.cap_tier_preset;
    diagnostics_snapshot["cap_tier_active"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.cap_tier_active
            : upload_pipeline.cap_tier_active;
    diagnostics_snapshot["effective_upload_cap_mb_per_frame"] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_frame);
    diagnostics_snapshot["effective_upload_cap_mb_per_slice"] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_slice);
    diagnostics_snapshot["effective_upload_cap_mb_per_second"] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_second);
    diagnostics_snapshot["effective_vram_budget_mb"] = effective_vram_budget_mb;
    diagnostics_snapshot["effective_vram_min_chunks"] = effective_vram_min_chunks;
    diagnostics_snapshot["effective_vram_max_chunks"] = effective_vram_max_chunks;
    diagnostics_snapshot["requested_vram_budget_mb"] = budget.vram_regulator.is_valid()
            ? static_cast<int64_t>(active_vram_caps.requested_budget_mb)
            : int64_t(0);
    diagnostics_snapshot["cap_source_upload_mb_per_frame"] = upload_pipeline.cap_source_upload_mb_per_frame;
    diagnostics_snapshot["cap_source_upload_mb_per_slice"] = upload_pipeline.cap_source_upload_mb_per_slice;
    diagnostics_snapshot["cap_source_upload_mb_per_second"] = upload_pipeline.cap_source_upload_mb_per_second;
    diagnostics_snapshot["cap_source_vram_budget_mb"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.source_budget_mb
            : String("project_default");
    diagnostics_snapshot["requested_cap_source_vram_budget_mb"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.requested_source_budget_mb
            : String("project_default");
    diagnostics_snapshot["cap_source_vram_min_chunks"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.source_min_chunks
            : String("project_default");
    diagnostics_snapshot["cap_source_vram_max_chunks"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.source_max_chunks
            : String("project_default");
    diagnostics_snapshot["vram_budget_capacity_verified"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.budget_capacity_verified
            : false;
    diagnostics_snapshot["vram_budget_unknown_capacity_fallback"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.budget_uses_unknown_capacity_fallback
            : false;
    diagnostics_snapshot["vram_budget_unverified"] = budget.vram_regulator.is_valid()
            ? active_vram_caps.budget_unverified
            : false;
    diagnostics_snapshot["upload_frame_cap_hit"] = upload_frame_cap_hit;
    diagnostics_snapshot["upload_slice_cap_hit"] = upload_slice_cap_hit;
    diagnostics_snapshot["upload_bandwidth_cap_hit"] = upload_bandwidth_cap_hit;
    diagnostics_snapshot["chunk_load_cap_hit"] = chunk_load_cap_hit;
    diagnostics_snapshot["vram_chunk_cap_hit"] = vram_chunk_cap_hit;
    diagnostics_snapshot["queue_pressure_frames"] = static_cast<int64_t>(diagnostics.queue_pressure_frames);
    diagnostics_snapshot["vram_cap_hit_frames"] = static_cast<int64_t>(diagnostics.vram_cap_hit_frames);
    diagnostics_snapshot["init_invalid_frames"] = static_cast<int64_t>(diagnostics.init_invalid_frames);
    diagnostics_snapshot["culling_empty_frames"] = static_cast<int64_t>(diagnostics.culling_empty_frames);
    diagnostics_snapshot["scheduler_stall_frames"] = static_cast<int64_t>(diagnostics.scheduler_stall_frames);
    diagnostics_snapshot["upload_stall_frames"] = static_cast<int64_t>(diagnostics.upload_stall_frames);
    diagnostics_snapshot["sync_fallback_stall_frames"] = static_cast<int64_t>(diagnostics.sync_fallback_stall_frames);
    diagnostics_snapshot["sustained_pressure_frames"] = static_cast<int64_t>(diagnostics.sustained_pressure_frames);
    diagnostics_snapshot["pressure_category"] = diagnostics.pressure_category;
    diagnostics_snapshot["scheduler_update_cpu_ms"] = scheduler.last_update_cpu_ms;
    diagnostics_snapshot["scheduler_visibility_cpu_ms"] = scheduler.last_visibility_cpu_ms;
    diagnostics_snapshot["scheduler_load_cpu_ms"] = scheduler.last_load_cpu_ms;
    diagnostics_snapshot["scheduler_build_visible_cpu_ms"] = scheduler.last_build_visible_cpu_ms;
    diagnostics_snapshot["scheduler_prefetch_cpu_ms"] = scheduler.last_prefetch_cpu_ms;
    diagnostics_snapshot["scheduler_sync_fallback_cpu_ms"] = scheduler.last_sync_fallback_cpu_ms;
    diagnostics_snapshot["scheduler_cpu_total_attributed_ms"] = scheduler.last_cpu_total_attributed_ms;
    diagnostics_snapshot["scheduler_cpu_unattributed_ms"] = scheduler.last_cpu_unattributed_ms;
    diagnostics_snapshot["pack_queue_latency_avg_ms"] = upload_pipeline.last_pack_queue_latency_avg_ms;
    diagnostics_snapshot["pack_queue_latency_max_ms"] = upload_pipeline.last_pack_queue_latency_max_ms;
    diagnostics_snapshot["upload_queue_latency_avg_ms"] = upload_pipeline.last_upload_queue_latency_avg_ms;
    diagnostics_snapshot["upload_queue_latency_max_ms"] = upload_pipeline.last_upload_queue_latency_max_ms;
    diagnostics_snapshot["pack_mutex_wait_avg_ms"] = upload_pipeline.last_pack_mutex_wait_avg_ms;
    diagnostics_snapshot["pack_mutex_wait_max_ms"] = upload_pipeline.last_pack_mutex_wait_max_ms;
    diagnostics_snapshot["visible_evict_fallback_attempts"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_attempts);
    diagnostics_snapshot["visible_evict_fallback_successes"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_successes);
    diagnostics_snapshot["invariant_slot_ownership_violations"] =
            static_cast<int64_t>(diagnostics.invariant_slot_ownership_violations);
    diagnostics_snapshot["invariant_upload_lifecycle_violations"] =
            static_cast<int64_t>(diagnostics.invariant_upload_lifecycle_violations);
    diagnostics_snapshot["invariant_generation_violations"] =
            static_cast<int64_t>(diagnostics.invariant_generation_violations);
    diagnostics_snapshot["integrity_mismatch_count"] =
            static_cast<int64_t>(diagnostics.integrity_mismatch_count);
    diagnostics_snapshot["last_invariant_context"] = diagnostics.last_invariant_context;
    diagnostics_snapshot["last_invariant_message"] = diagnostics.last_invariant_message;
    diagnostics_snapshot["last_integrity_mismatch_message"] = diagnostics.last_integrity_mismatch_message;
    diagnostics_snapshot["atlas_generation"] = static_cast<int64_t>(global_atlas_registry.get_atlas_generation());
    diagnostics_snapshot["request_generation"] = static_cast<int64_t>(asset_registry.request_generation);

    return diagnostics_snapshot;
}

RID GaussianStreamingSystem::get_frame_buffer() const {
    return persistent_buffer;
}

uint64_t GaussianStreamingSystem::get_vram_usage() const {
    return _get_total_vram_usage_bytes();
}

uint32_t GaussianStreamingSystem::get_pending_pack_jobs() {
    return upload_pipeline.get_pack_queue_depth_cached();
}

uint32_t GaussianStreamingSystem::get_pending_upload_jobs() {
    return upload_pipeline.get_upload_queue_depth_cached();
}

uint32_t GaussianStreamingSystem::get_pending_upload_retirement_slots() const {
    return budget.pending_upload_slots;
}

uint64_t GaussianStreamingSystem::get_pending_upload_retirement_bytes() const {
    return budget.pending_upload_bytes;
}

uint32_t GaussianStreamingSystem::get_visible_count() const {
    const FrameData &frame = frame_data[current_frame_idx];
    uint32_t count = 0;
    for (uint32_t chunk_idx : frame.visible_chunks) {
        if (chunk_idx >= chunks.size()) {
            continue;
        }
        count += chunks[chunk_idx].count;
    }
    return count;
}

LocalVector<Gaussian> GaussianStreamingSystem::get_visible_gaussians() const {
    LocalVector<Gaussian> visible_gaussians;
    const FrameData &frame = frame_data[current_frame_idx];
    const LODConfig &lod_config = _get_lod_config();
    const AtlasAssetState *primary_asset = asset_registry.atlas_assets.getptr(PRIMARY_ASSET_ID);
    const Ref<ChunkPayloadSource> payload_source = primary_asset ? primary_asset->payload_source : Ref<ChunkPayloadSource>();
    if (source_data.is_null() && (payload_source.is_null() || !payload_source->is_valid())) {
        return visible_gaussians;
    }

    // Calculate total count after LOD reduction (using effective_count)
    uint32_t total_count = get_effective_splat_count();
    visible_gaussians.reserve(total_count);

    for (uint32_t chunk_idx : frame.visible_chunks) {
        if (chunk_idx >= chunks.size()) {
            continue;
        }
        const StreamingChunk &chunk = chunks[chunk_idx];
        uint32_t skip_factor = MAX(1u, (uint32_t)chunk.splat_skip_factor);

        LocalVector<Gaussian> payload_gaussians;
        LocalVector<Vector3> payload_sh;
        uint32_t payload_sh_first = 0;
        uint32_t payload_sh_high = 0;
        if (source_data.is_null()) {
            bool snapshot_ok = false;
            if (chunk.source_index_remapped) {
                LocalVector<uint32_t> source_indices;
                source_indices.resize(chunk.count);
                for (uint32_t i = 0; i < chunk.count; i++) {
                    uint32_t source_index = 0;
                    if (!_resolve_primary_chunk_source_index(chunk, i, source_index)) {
                        source_indices.clear();
                        break;
                    }
                    source_indices[i] = source_index;
                }
                snapshot_ok = !source_indices.is_empty() &&
                        payload_source->capture_indexed_chunk_snapshot(source_indices.ptr(), chunk.count,
                                payload_gaussians, payload_sh, payload_sh_first, payload_sh_high);
            } else {
                snapshot_ok = payload_source->capture_chunk_snapshot(chunk.start_idx, chunk.count,
                        payload_gaussians, payload_sh, payload_sh_first, payload_sh_high);
            }
            if (!snapshot_ok || payload_gaussians.size() < chunk.count) {
                continue;
            }
        }

        // Apply LOD-based splat skipping: render every Nth splat
        for (uint32_t i = 0; i < chunk.count; i += skip_factor) {
            Gaussian g;
            if (source_data.is_valid()) {
                uint32_t source_index = 0;
                if (!_resolve_primary_chunk_source_index(chunk, i, source_index)) {
                    continue;
                }
                g = source_data->get_gaussian(source_index);
            } else {
                g = payload_gaussians[i];
            }

            // Apply LOD-based opacity fade if enabled
            if (lod_config.opacity_fade_enabled && chunk.opacity_multiplier < 1.0f) {
                g.opacity *= chunk.opacity_multiplier;
            }

            visible_gaussians.push_back(g);
        }
    }

    return visible_gaussians;
}

LocalVector<uint32_t> GaussianStreamingSystem::get_visible_indices() const {
    LocalVector<uint32_t> indices;
    const FrameData &frame = frame_data[current_frame_idx];

    // Calculate total count after LOD reduction
    uint32_t total_count = get_effective_splat_count();
    indices.reserve(total_count);

    for (uint32_t chunk_idx : frame.visible_chunks) {
        const StreamingChunk &chunk = chunks[chunk_idx];
        int skip_factor = chunk.splat_skip_factor;

        // CRITICAL FIX: Return BUFFER-SPACE indices, not source-space indices!
        // The shader uses identity mapping (gaussian_idx = global_idx), so indices
        // must match the actual GPU buffer layout (buffer_slot * CHUNK_SIZE + offset).
        // Previously this returned chunk.start_idx (source space) which only worked
        // when chunks loaded in sequential order - causing rectangular holes otherwise.
        uint32_t buffer_base = chunk.buffer_slot * CHUNK_SIZE;

        // Apply LOD-based splat skipping: return every Nth index
        for (uint32_t i = 0; i < chunk.count; i += skip_factor) {
            indices.push_back(buffer_base + i);
        }
    }

    return indices;
}

Dictionary GaussianStreamingSystem::get_task_debug_state() const {
    if (memory_stream_proxy.is_null()) {
        return Dictionary();
    }
    return memory_stream_proxy->get_task_debug_state();
}

Dictionary GaussianStreamingSystem::get_streaming_analytics() const {
    return analytics_snapshot;
}

bool GaussianStreamingSystem::is_runtime_capacity_zero() const {
    if (get_effective_max_chunks() == 0) {
        return true;
    }
    if (_compute_runtime_chunk_capacity_limit() == 0) {
        return true;
    }
    return atlas_allocator.get_capacity() == 0;
}

bool GaussianStreamingSystem::is_persistent_buffer_invalid() const {
    return !persistent_buffer.is_valid() || persistent_buffer_size == 0;
}

bool GaussianStreamingSystem::is_runtime_ready(String *r_reason) const {
	if (!streaming_initialized) {
		if (r_reason) {
			*r_reason = "streaming system is not initialized";
		}
		return false;
	}
	if (!budget.vram_regulator.is_valid()) {
		if (r_reason) {
			*r_reason = "VRAM regulator is not initialized";
		}
		return false;
	}
	if (is_runtime_capacity_zero()) {
		if (r_reason) {
			*r_reason = "runtime chunk capacity resolved to 0";
		}
		return false;
	}
	if (is_persistent_buffer_invalid()) {
		if (r_reason) {
			*r_reason = "persistent streaming buffer is invalid";
		}
		return false;
	}
	return true;
}

uint32_t GaussianStreamingSystem::get_registered_asset_count_with_data() const {
	uint32_t count = 0;
	for (const KeyValue<uint32_t, AtlasAssetState> &E : asset_registry.atlas_assets) {
		if (E.value.data.is_valid() || (E.value.payload_source.is_valid() && E.value.payload_source->is_valid())) {
			count++;
		}
	}
	return count;
}

bool GaussianStreamingSystem::_is_chunk_in_frustum(const AABB &p_bounds, const Vector<Plane> &p_frustum_planes) const {
    return visibility.is_chunk_in_frustum(p_bounds, p_frustum_planes);
}

void GaussianStreamingSystem::_update_culling_config_from_project_settings() {
    visibility.update_culling_config_from_project_settings();
}

void GaussianStreamingSystem::_load_streaming_tuning_config_from_project_settings() {
    eviction_controller.load_streaming_tuning_config_from_project_settings();
    upload_pipeline.load_streaming_tuning_config_from_project_settings(*this);
}

void GaussianStreamingSystem::_reload_debug_logging_config() {
#ifdef GS_SILENCE_LOGS
    debug_logging_enabled = false;
    debug_frame_log_frequency = 0;
#else
    debug_logging_enabled = GaussianSplatting::is_debug_frame_logging_enabled();
    debug_frame_log_frequency = GaussianSplatting::get_debug_frame_log_frequency(0);
    if (debug_logging_enabled && debug_frame_log_frequency <= 0) {
        debug_frame_log_frequency = 1;
    }
#endif
    upload_pipeline.telemetry.set_enabled(debug_logging_enabled);
}

void GaussianStreamingSystem::_start_pack_threads() {
    upload_pipeline.start_pack_threads(*this);
}

void GaussianStreamingSystem::_stop_pack_threads() {
    upload_pipeline.stop_pack_threads(*this);
}

bool GaussianStreamingSystem::_enqueue_chunk_load_request(
        uint32_t asset_id, uint32_t chunk_idx, bool can_async_pack, bool prioritize_sync_fallback) {
    if (can_async_pack) {
        return _queue_chunk_load(asset_id, chunk_idx);
    }
    return _enqueue_sync_fallback_chunk_load(asset_id, chunk_idx, prioritize_sync_fallback);
}

bool GaussianStreamingSystem::_should_force_sync_fallback_for_async_stall(
        uint32_t pack_queue_depth, uint32_t upload_queue_depth) {
    const bool async_threads_available =
            upload_pipeline.async_pack_enabled && upload_pipeline.pack_thread_running.load();
    if (!async_threads_available) {
        return false;
    }

    const uint32_t sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    if (scheduler.force_sync_fallback_due_to_async_stall &&
            pack_queue_depth == 0 &&
            upload_queue_depth == 0 &&
            sync_fallback_queue_depth == 0) {
        scheduler.force_sync_fallback_due_to_async_stall = false;
    }

    // Original narrow signature: pack queue has work but nothing reaches
    // the upload queue — pack thread may be wedged.
    if (!scheduler.force_sync_fallback_due_to_async_stall &&
            diagnostics.upload_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES &&
            pack_queue_depth > 0 &&
            upload_queue_depth == 0) {
        scheduler.force_sync_fallback_due_to_async_stall = true;
    }

    // Broader signature: upload queue has work but nothing completes —
    // GPU upload path may be blocked or starved.
    if (!scheduler.force_sync_fallback_due_to_async_stall &&
            diagnostics.upload_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES &&
            upload_queue_depth > 0 &&
            upload_pipeline.last_upload_chunks == 0) {
        scheduler.force_sync_fallback_due_to_async_stall = true;
    }

    return scheduler.force_sync_fallback_due_to_async_stall;
}

bool GaussianStreamingSystem::_can_use_async_pack_path(
        uint32_t pack_queue_depth, uint32_t upload_queue_depth) {
    const bool async_threads_available =
            upload_pipeline.async_pack_enabled && upload_pipeline.pack_thread_running.load();
    if (!async_threads_available) {
        return false;
    }
    return !_should_force_sync_fallback_for_async_stall(pack_queue_depth, upload_queue_depth);
}

uint32_t GaussianStreamingSystem::_get_sync_fallback_queue_depth() const {
    return scheduler.sync_fallback_chunk_load_queue_read_idx < scheduler.sync_fallback_chunk_load_queue.size()
            ? (scheduler.sync_fallback_chunk_load_queue.size() - scheduler.sync_fallback_chunk_load_queue_read_idx)
            : 0;
}

void GaussianStreamingSystem::_compact_sync_fallback_queue() {
    if (scheduler.sync_fallback_chunk_load_queue_read_idx >= scheduler.sync_fallback_chunk_load_queue.size()) {
        scheduler.sync_fallback_chunk_load_queue.clear();
        scheduler.sync_fallback_chunk_load_set.clear();
        scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
        return;
    }
    if (scheduler.sync_fallback_chunk_load_queue_read_idx >= SchedulerState::SYNC_FALLBACK_QUEUE_COMPACT_MIN_PREFIX &&
            scheduler.sync_fallback_chunk_load_queue_read_idx * 2 >= scheduler.sync_fallback_chunk_load_queue.size()) {
        const uint32_t remaining = scheduler.sync_fallback_chunk_load_queue.size() -
                scheduler.sync_fallback_chunk_load_queue_read_idx;
        for (uint32_t i = 0; i < remaining; i++) {
            scheduler.sync_fallback_chunk_load_queue[i] =
                    scheduler.sync_fallback_chunk_load_queue[scheduler.sync_fallback_chunk_load_queue_read_idx + i];
        }
        scheduler.sync_fallback_chunk_load_queue.resize(remaining);
        scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
    }
}

bool GaussianStreamingSystem::_enqueue_sync_fallback_chunk_load(uint32_t asset_id, uint32_t chunk_idx, bool prioritize) {
    AtlasAssetState *asset = _get_asset_state(asset_id);
    if (!asset || (!asset->data.is_valid() && !asset->payload_source.is_valid())) {
        return false;
    }

    LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
    if (chunk_idx >= asset_chunks.size()) {
        return false;
    }

    const StreamingChunk &chunk = asset_chunks[chunk_idx];
    if (chunk.is_loaded || chunk.upload_pending) {
        return false;
    }

    const uint64_t chunk_key = _make_chunk_key(asset_id, chunk_idx);
    if (scheduler.sync_fallback_chunk_load_set.has(chunk_key)) {
        return true;
    }

    if (scheduler.sync_fallback_chunk_load_queue_read_idx > 0) {
        _compact_sync_fallback_queue();
    }

    uint32_t queue_depth = _get_sync_fallback_queue_depth();
    if (scheduler.max_sync_fallback_queue_size > 0 &&
            queue_depth >= scheduler.max_sync_fallback_queue_size) {
        StreamingQueuePressureController::mark_latched_state(
                upload_pipeline.queue_pressure_active,
                upload_pipeline.queue_pressure_source,
                upload_pipeline.queue_pressure_reason,
                StreamingQueuePressureController::SOURCE_SYNC,
                StreamingQueuePressureController::REASON_SYNC_QUEUE_CAP);
        _validate_queue_pressure_latched_state(
                upload_pipeline.queue_pressure_active,
                upload_pipeline.queue_pressure_source,
                upload_pipeline.queue_pressure_reason,
                "_enqueue_sync_fallback_chunk_load.queue_cap");
        if (!prioritize || queue_depth == 0) {
            scheduler.last_sync_fallback_dropped_count++;
            return false;
        }
        const uint32_t tail_idx = scheduler.sync_fallback_chunk_load_queue.size() - 1;
        const uint64_t dropped_key = scheduler.sync_fallback_chunk_load_queue[tail_idx];
        scheduler.sync_fallback_chunk_load_queue.remove_at(tail_idx);
        scheduler.sync_fallback_chunk_load_set.erase(dropped_key);
        scheduler.last_sync_fallback_dropped_count++;
        queue_depth--;
    }

    scheduler.sync_fallback_chunk_load_queue.push_back(chunk_key);
    scheduler.sync_fallback_chunk_load_set.insert(chunk_key);
    if (prioritize && queue_depth > 0) {
        const uint32_t tail_idx = scheduler.sync_fallback_chunk_load_queue.size() - 1;
        const uint32_t front_idx = scheduler.sync_fallback_chunk_load_queue_read_idx;
        uint64_t *queue_write = scheduler.sync_fallback_chunk_load_queue.ptr();
        const uint64_t front_key = queue_write[front_idx];
        queue_write[front_idx] = queue_write[tail_idx];
        queue_write[tail_idx] = front_key;
    }
    scheduler.last_sync_fallback_enqueued_count++;
    scheduler.last_sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    return true;
}

uint32_t GaussianStreamingSystem::_drain_sync_fallback_chunk_loads(
        uint32_t effective_max, uint32_t &evictions_left, bool &eviction_blocked) {
    const uint32_t queue_depth_before = _get_sync_fallback_queue_depth();
    scheduler.last_sync_fallback_queue_depth = queue_depth_before;
    if (queue_depth_before == 0) {
        return 0;
    }

    uint32_t drain_budget = scheduler.max_sync_fallback_loads_per_frame;
    if (upload_pipeline.max_chunk_loads_per_frame > 0) {
        if (budget.chunks_loaded_this_frame >= upload_pipeline.max_chunk_loads_per_frame) {
            return 0;
        }
        const uint32_t remaining_frame_budget = upload_pipeline.max_chunk_loads_per_frame - budget.chunks_loaded_this_frame;
        drain_budget = MIN(drain_budget, remaining_frame_budget);
    }
    if (drain_budget == 0) {
        return 0;
    }

    const float lod_mult = budget.vram_regulator.is_valid()
            ? budget.vram_regulator->get_lod_distance_multiplier()
            : 1.0f;
    const float primary_load_threshold = STREAMING_LOAD_DISTANCE_BASE / lod_mult;
    const bool primary_prefetch_enabled = visibility.predictive_prefetch_enabled &&
            visibility.prefetch_lookahead_distance > 0.0f &&
            visibility.camera_tracker.has_previous_position &&
            visibility.camera_tracker.velocity.length_squared() >= 0.01f;
    const Vector3 primary_camera_pos = visibility.camera_tracker.last_position;
    const float primary_prefetch_threshold_sq = visibility.prefetch_lookahead_distance *
            visibility.prefetch_lookahead_distance * 2.25f;
    const auto is_primary_chunk_relevant = [&](const StreamingChunk &p_chunk) -> bool {
        if (p_chunk.is_visible && p_chunk.distance < primary_load_threshold) {
            return true;
        }
        if (!primary_prefetch_enabled) {
            return false;
        }
        static const float horizon_multipliers[] = { 0.5f, 1.0f, 2.0f };
        for (float multiplier : horizon_multipliers) {
            const Vector3 predicted_pos = visibility.camera_tracker.predict_position(
                    primary_camera_pos,
                    visibility.prefetch_lookahead_distance * multiplier);
            if ((predicted_pos - p_chunk.center).length_squared() < primary_prefetch_threshold_sq) {
                return true;
            }
        }
        return false;
    };

    ResidencyBudgetController::AdmissionFrameBudget admission_budget =
            ResidencyBudgetController::make_frame_budget(effective_max, evictions_left, eviction_blocked);
    uint32_t drained = 0;
    uint32_t attempted = 0;
    uint32_t scanned = 0;
    const uint32_t scan_budget = MAX<uint32_t>(drain_budget, drain_budget * 8u);
    const uint32_t initial_queue_end = scheduler.sync_fallback_chunk_load_queue.size();
    while (attempted < drain_budget &&
            scanned < scan_budget &&
            scheduler.sync_fallback_chunk_load_queue_read_idx < initial_queue_end) {
        scanned++;
        const uint64_t chunk_key = scheduler.sync_fallback_chunk_load_queue[scheduler.sync_fallback_chunk_load_queue_read_idx++];
        scheduler.sync_fallback_chunk_load_set.erase(chunk_key);
        const uint32_t asset_id = uint32_t(chunk_key >> 32);
        const uint32_t chunk_idx = uint32_t(chunk_key & 0xffffffffu);

        AtlasAssetState *asset = _get_asset_state(asset_id);
        if (!asset || (!asset->data.is_valid() && !asset->payload_source.is_valid())) {
            continue;
        }

        LocalVector<StreamingChunk> &asset_chunks = _get_asset_chunks(*asset);
        if (chunk_idx >= asset_chunks.size()) {
            continue;
        }

        StreamingChunk &chunk = asset_chunks[chunk_idx];
        if (chunk.is_loaded || chunk.upload_pending) {
            continue;
        }
        const bool explicitly_requested = _is_requested_chunk_in_current_generation(*asset, chunk_idx);
        bool enforce_vram_regulator_gate = true;
        if (asset_id == PRIMARY_ASSET_ID) {
            if (!explicitly_requested && !is_primary_chunk_relevant(chunk)) {
                scheduler.last_sync_fallback_stalled_count++;
                continue;
            }
            if (explicitly_requested) {
                enforce_vram_regulator_gate = false;
            }
        } else {
            enforce_vram_regulator_gate = false;
            if (!explicitly_requested) {
                continue;
            }
        }
        attempted++;

        ResidencyBudgetController::AdmissionPolicy admission_policy;
        admission_policy.can_replace_without_eviction = false;
        admission_policy.enforce_vram_regulator_gate =
                enforce_vram_regulator_gate && budget.vram_regulator.is_valid();
        const uint32_t reserved_chunks = _get_reserved_chunk_count();
        admission_policy.vram_regulator_allows_load =
                !admission_policy.enforce_vram_regulator_gate ||
                budget.vram_regulator->can_load_more_chunks(reserved_chunks);
        _try_grow_persistent_buffer_for_atlas_pressure(
                reserved_chunks,
                get_regulated_max_chunks(),
                admission_policy.enforce_vram_regulator_gate,
                admission_policy.vram_regulator_allows_load);
        admission_policy.atlas_slots_full = !atlas_allocator.has_free_slots();

        const ResidencyBudgetController::AdmissionGate admission_gate =
                ResidencyBudgetController::compute_admission_gate(
                        reserved_chunks,
                        admission_budget,
                        admission_policy);
        const ResidencyBudgetController::AdmissionDecision decision = admission_gate.decision;
        if (decision == ResidencyBudgetController::AdmissionDecision::Skip) {
            scheduler.last_sync_fallback_stalled_count++;
            _update_requested_chunk_state(*asset, chunk_idx,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                    ERR_BUSY);
            _enqueue_sync_fallback_chunk_load(asset_id, chunk_idx, asset_id != PRIMARY_ASSET_ID);
            continue;
        }
        if (decision == ResidencyBudgetController::AdmissionDecision::EvictThenLoad) {
            bool visible_fallback_attempted = false;
            EvictionResult result = _evict_for_admission_gate(admission_gate, visible_fallback_attempted);
            if (visible_fallback_attempted) {
                diagnostics.visible_evict_fallback_attempts++;
                if (result == EvictionResult::EvictedNonVisible || result == EvictionResult::EvictedVisible) {
                    diagnostics.visible_evict_fallback_successes++;
                }
            }
            if (result == EvictionResult::EvictedNonVisible || result == EvictionResult::EvictedVisible) {
                eviction_controller.record_eviction_result(result);
                ResidencyBudgetController::note_successful_eviction(admission_budget);
            } else {
                ResidencyBudgetController::note_blocked_eviction(admission_budget);
                scheduler.last_sync_fallback_stalled_count++;
                _update_requested_chunk_state(*asset, chunk_idx,
                        GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                        GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                        ERR_BUSY);
                _enqueue_sync_fallback_chunk_load(asset_id, chunk_idx, asset_id != PRIMARY_ASSET_ID);
                continue;
            }
        }

        const RequestedChunkState *request_state = asset->requested_chunk_state.getptr(chunk_idx);
        chunk.explicit_request_generation = request_state ? request_state->stamp : uint64_t(0);
        const Error load_error = _load_chunk(asset_id, chunk_idx);
        if (chunk.is_loaded) {
            _update_requested_chunk_state(*asset, chunk_idx,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_SATISFIED,
                    OK);
            drained++;
            scheduler.last_sync_fallback_drained_count++;
            continue;
        }

        if (chunk.upload_pending) {
            _update_requested_chunk_state(*asset, chunk_idx,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_QUEUED,
                    OK);
        } else if (_is_terminal_residency_request_error(load_error)) {
            scheduler.last_sync_fallback_stalled_count++;
            _update_requested_chunk_state(*asset, chunk_idx,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_FAILED,
                    load_error);
            chunk.explicit_request_generation = 0;
        } else {
            scheduler.last_sync_fallback_stalled_count++;
            _update_requested_chunk_state(*asset, chunk_idx,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                    GaussianStreamingTypes::RESIDENCY_REQUEST_STATE_DEFERRED,
                    load_error == OK ? ERR_BUSY : load_error);
            _enqueue_sync_fallback_chunk_load(asset_id, chunk_idx, asset_id != PRIMARY_ASSET_ID);
        }
    }

    _compact_sync_fallback_queue();
    scheduler.last_sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    evictions_left = admission_budget.evictions_left;
    eviction_blocked = admission_budget.eviction_blocked;

    // Track consecutive frames where the sync fallback queue has entries but
    // drains zero chunks. When the staleness limit is reached, flush the
    // queue to prevent indefinite re-enqueue loops under sustained pressure.
    if (drained == 0 && scheduler.last_sync_fallback_queue_depth > 0) {
        scheduler.sync_fallback_no_progress_frames++;
        if (scheduler.sync_fallback_no_progress_frames >= SchedulerState::SYNC_FALLBACK_STALENESS_LIMIT_FRAMES) {
            WARN_PRINT(vformat("[Streaming] Sync fallback queue stale for %d frames — flushing %d entries.",
                    scheduler.sync_fallback_no_progress_frames,
                    scheduler.last_sync_fallback_queue_depth));
            scheduler.sync_fallback_chunk_load_queue.clear();
            scheduler.sync_fallback_chunk_load_set.clear();
            scheduler.sync_fallback_chunk_load_queue_read_idx = 0;
            scheduler.sync_fallback_no_progress_frames = 0;
            scheduler.last_sync_fallback_queue_depth = 0;
        }
    } else {
        scheduler.sync_fallback_no_progress_frames = 0;
    }

    return drained;
}

bool GaussianStreamingSystem::_queue_chunk_load(uint32_t chunk_idx) {
    return _queue_chunk_load(PRIMARY_ASSET_ID, chunk_idx);
}

bool GaussianStreamingSystem::_queue_chunk_load(uint32_t asset_id, uint32_t chunk_idx) {
    return upload_pipeline.queue_chunk_load(*this, asset_id, chunk_idx);
}

void GaussianStreamingSystem::_process_upload_queue() {
    upload_pipeline.process_upload_queue(*this);
}

void GaussianStreamingSystem::_clear_pending_uploads() {
    upload_pipeline.clear_pending_uploads(*this);
}

void GaussianStreamingSystem::_apply_config_overrides() {
    if (!config_overrides_active) {
        if (budget.vram_regulator.is_valid() && budget.vram_regulator->is_config_override_active()) {
            budget.vram_regulator->clear_config_override();
        }
        return;
    }

    if (config_overrides.override_chunk_culling) {
        visibility.chunk_frustum_culling_enabled = config_overrides.chunk_frustum_culling_enabled;
        visibility.chunk_frustum_padding = MAX(1.0f, config_overrides.chunk_frustum_padding);
    }

    if (config_overrides.override_prefetch) {
        visibility.predictive_prefetch_enabled = config_overrides.predictive_prefetch_enabled;
        visibility.prefetch_lookahead_distance = MAX(0.0f, config_overrides.prefetch_lookahead_distance);
    }

    if (config_overrides.override_streaming_tuning) {
        upload_pipeline.max_chunk_loads_per_frame = config_overrides.max_chunk_loads_per_frame;
    }

    if (config_overrides.override_lod_blend) {
        visibility.lod_blend_config = config_overrides.lod_blend_config;
        visibility.lod_blend_config.blend_distance = MAX(0.1f, visibility.lod_blend_config.blend_distance);
        visibility.lod_blend_config.hysteresis_zone = MAX(0.0f, visibility.lod_blend_config.hysteresis_zone);
    }

    if (budget.vram_regulator.is_valid()) {
        if (config_overrides.override_vram_budget) {
            VRAMBudgetConfig override_config = config_overrides.vram_budget_config;
            override_config.cap_tier_preset = "override";
            override_config.cap_tier_active = false;
            override_config.source_budget_mb = "runtime_override";
            override_config.source_min_chunks = "runtime_override";
            override_config.source_max_chunks = "runtime_override";
            budget.vram_regulator->set_config_override(override_config);
        } else if (budget.vram_regulator->is_config_override_active()) {
            budget.vram_regulator->clear_config_override();
        }
    }
}

const LODConfig &GaussianStreamingSystem::_get_lod_config() const {
    if (config_overrides_active && config_overrides.override_lod_config) {
        return config_overrides.lod_config;
    }
    return g_lod_config;
}

Dictionary GaussianStreamingSystem::get_chunk_culling_stats() const {
    Dictionary stats;
    const bool strict_layout_validation = _layout_hint_strict_validation_enabled();
    const Dictionary layout_hint_validation = _layout_hint_build_snapshot(this, strict_layout_validation);
    stats["total_chunks"] = visibility.culling_stats.total_chunks;
    stats["visible_chunks"] = visibility.culling_stats.visible_chunks;
    stats["frustum_culled_chunks"] = visibility.culling_stats.frustum_culled_chunks;
    stats["loaded_chunks"] = visibility.culling_stats.loaded_chunks;
    stats["resident_chunks"] = visibility.culling_stats.resident_chunks;
    stats["visibility_flag_reset_scan_count"] = visibility.culling_stats.visibility_flag_reset_scan_count;
    stats["lod_parameter_update_scan_count"] = visibility.culling_stats.lod_parameter_update_scan_count;
    stats["lod_blend_update_scan_count"] = visibility.culling_stats.lod_blend_update_scan_count;
    stats["primary_eviction_scan_count"] = static_cast<int64_t>(scheduler.last_primary_eviction_scan_count);
    stats["primary_eviction_candidate_count"] = static_cast<int64_t>(scheduler.last_primary_eviction_candidate_count);
    stats["non_primary_eviction_scan_count"] = static_cast<int64_t>(scheduler.last_non_primary_scan_count);
    stats["non_primary_eviction_candidate_count"] = static_cast<int64_t>(scheduler.last_non_primary_eviction_candidate_count);
    stats["atlas_published_chunks"] = global_atlas_registry.get_atlas_published_chunks();
    stats["culling_enabled"] = visibility.chunk_frustum_culling_enabled;
    stats["frustum_padding"] = visibility.chunk_frustum_padding;
    stats["zero_visible_consecutive_frames"] = visibility.zero_visible_recovery.zero_visible_consecutive_frames;
    stats["zero_visible_recoveries_triggered"] = (int)visibility.zero_visible_recovery.recoveries_triggered;
    stats["zero_visible_stall_detections"] = (int)visibility.zero_visible_recovery.stall_detections;
    stats["zero_visible_recovery_mode"] =
            visibility.zero_visible_recovery.mode == ZeroVisibleRecoveryMode::PERSISTENT ? "persistent" : "startup_only";
    stats["runtime_capacity_zero"] = is_runtime_capacity_zero();
    stats["runtime_buffer_invalid"] = is_persistent_buffer_invalid();
    stats["invalid_camera_input_events"] = (int)invalid_camera_input_events;
    stats["visible_evict_fallback_attempts"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_attempts);
    stats["visible_evict_fallback_successes"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_successes);
    stats["layout_hint_validation"] = layout_hint_validation;
    stats["layout_hint_strict_mode_enabled"] = strict_layout_validation;
    stats["layout_hint_fallback_total"] = layout_hint_validation.get("fallback_total", int64_t(0));
    stats["layout_hint_fallback_io_total"] = layout_hint_validation.get("fallback_io_total", int64_t(0));
    stats["layout_hint_fallback_primary_total"] = layout_hint_validation.get("fallback_primary_total", int64_t(0));
    stats["layout_hint_strict_failure_total"] = layout_hint_validation.get("strict_failure_total", int64_t(0));
    stats["layout_hint_last_reason"] = layout_hint_validation.get("last_reason", String("none"));
    stats["layout_hint_last_reason_category"] = layout_hint_validation.get("last_reason_category", String("other"));
    stats["layout_hint_last_context"] = layout_hint_validation.get("last_context", String("none"));
    stats["layout_hint_last_usage"] = layout_hint_validation.get("last_usage", String("none"));
    stats["vram_payload_mb"] = double(budget.vram_usage) / (1024.0 * 1024.0);
    stats["vram_overhead_mb"] = double(_get_auxiliary_vram_overhead_bytes()) / (1024.0 * 1024.0);
    stats["cull_effectiveness"] = visibility.culling_stats.total_chunks > 0
            ? float(visibility.culling_stats.frustum_culled_chunks) / float(visibility.culling_stats.total_chunks)
            : 0.0f;
    stats["primary_spatial_chunking_enabled"] = primary_chunk_layout_metrics.spatial_partition_enabled;
    stats["primary_chunk_source_index_count"] = static_cast<int64_t>(primary_chunk_layout_metrics.source_index_count);
    stats["primary_chunk_avg_radius_ratio"] = primary_chunk_layout_metrics.avg_chunk_radius_ratio;
    stats["primary_chunk_max_radius_ratio"] = primary_chunk_layout_metrics.max_chunk_radius_ratio;
    stats["primary_chunk_bounds_volume_ratio"] = primary_chunk_layout_metrics.bounds_volume_ratio;

    // Calculate culling efficiency
    if (visibility.culling_stats.total_chunks > 0) {
        stats["cull_ratio"] = float(visibility.culling_stats.frustum_culled_chunks) / float(visibility.culling_stats.total_chunks);
    } else {
        stats["cull_ratio"] = 0.0f;
    }

    return stats;
}

void GaussianStreamingSystem::_load_prefetch_config_from_project_settings() {
    visibility.load_prefetch_config_from_project_settings();
}

uint32_t GaussianStreamingSystem::_prefetch_chunks_at_predicted_position(const Vector3 &predicted_pos,
        uint32_t available_slots, uint32_t load_budget, uint32_t max_scan_budget) {
    return visibility.prefetch_chunks_at_predicted_position(*this, predicted_pos, available_slots, load_budget, max_scan_budget);
}

Dictionary GaussianStreamingSystem::get_vram_debug_stats() const {
    return budget.get_vram_debug_stats();
}

bool GaussianStreamingSystem::is_vram_budget_warning_active() const {
    return budget.is_vram_budget_warning_active();
}

uint32_t GaussianStreamingSystem::get_effective_max_chunks() const {
    const uint32_t regulated_max = budget.get_effective_max_chunks();
    if (!streaming_initialized) {
        return regulated_max;
    }
    const uint32_t runtime_capacity_max = _compute_runtime_chunk_capacity_limit();
    if (runtime_capacity_max == 0) {
        return 0;
    }
    return MIN(regulated_max, runtime_capacity_max);
}

uint32_t GaussianStreamingSystem::get_regulated_max_chunks() const {
    return budget.get_effective_max_chunks();
}

Dictionary GaussianStreamingTypes::BudgetState::get_vram_debug_stats() const {
    if (vram_regulator.is_valid()) {
        return vram_regulator->get_debug_stats_dictionary();
    }
    return Dictionary();
}

bool GaussianStreamingTypes::BudgetState::is_vram_budget_warning_active() const {
    if (vram_regulator.is_valid()) {
        return vram_regulator->is_budget_warning_active();
    }
    return false;
}

uint32_t GaussianStreamingTypes::BudgetState::get_effective_max_chunks() const {
    if (vram_regulator.is_valid()) {
        return vram_regulator->get_current_max_chunks();
    }
    return GaussianStreamingSystem::MAX_CHUNKS_IN_VRAM;
}

float GaussianStreamingSystem::get_visible_count_change_ratio() const {
    return visibility.get_visible_count_change_ratio();
}

float GaussianStreamingSystem::get_visible_chunk_change_ratio() const {
    // Alias — same metric viewed from chunk perspective.
    return get_visible_count_change_ratio();
}

float GaussianStreamingSystem::get_effective_count_change_ratio() const {
    return visibility.get_effective_count_change_ratio(eviction_controller.get_visible_chunks_evicted_this_frame());
}

uint32_t GaussianStreamingSystem::get_buffer_capacity_splats() const {
    return atlas_allocator.get_capacity() * CHUNK_SIZE;
}

bool GaussianStreamingSystem::map_buffer_index_to_source(uint32_t buffer_index, uint32_t &out_source_index) const {
    // Walk loaded chunks to find which chunk owns this buffer position and
    // map back to the original GaussianData index.
    for (uint32_t i = 0; i < chunks.size(); i++) {
        const StreamingChunk &chunk = chunks[i];
        if (!chunk.is_loaded || chunk.buffer_slot == UINT32_MAX) {
            continue;
        }
        uint32_t slot_start = chunk.buffer_slot * CHUNK_SIZE;
        uint32_t slot_end = slot_start + chunk.count;
        if (buffer_index >= slot_start && buffer_index < slot_end) {
            uint32_t offset = buffer_index - slot_start;
            out_source_index = chunk.start_idx + offset;
            return true;
        }
    }
    return false;
}
