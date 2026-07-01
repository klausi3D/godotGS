#include "streaming_layout_hint.h"

#include "gs_project_settings.h"

#include "core/config/project_settings.h"
#include "core/string/ustring.h"

#include <algorithm>
#include <cstdint>

namespace gs_layout_hint {

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

} // namespace gs_layout_hint
