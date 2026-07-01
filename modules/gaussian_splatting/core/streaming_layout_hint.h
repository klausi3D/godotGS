#ifndef STREAMING_LAYOUT_HINT_H
#define STREAMING_LAYOUT_HINT_H

#include "gaussian_streaming.h"

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

#include <cstdint>

namespace gs_layout_hint {

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

uint64_t _layout_hint_state_key(const GaussianStreamingSystem *p_system);
LayoutHintFailureState &_layout_hint_get_or_create_state(const GaussianStreamingSystem *p_system);
const LayoutHintFailureState *_layout_hint_get_state(const GaussianStreamingSystem *p_system);
void _layout_hint_reset_state(const GaussianStreamingSystem *p_system);
bool _layout_hint_strict_validation_enabled();
const char *_layout_hint_usage_code(LayoutHintUsage p_usage);
const char *_layout_hint_reason_code(LayoutHintFailureReason p_reason);
LayoutHintFailureCategory _layout_hint_reason_category(LayoutHintFailureReason p_reason);
const char *_layout_hint_category_code(LayoutHintFailureCategory p_category);
String _layout_hint_failure_detail(const LayoutHintValidationFailure &p_failure);
void _layout_hint_set_last_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage,
        const LayoutHintValidationFailure &p_failure);
void _layout_hint_clear_last_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage);
LayoutHintValidationFailure _layout_hint_get_last_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage);
String _layout_hint_record_failure(const GaussianStreamingSystem *p_system, LayoutHintUsage p_usage,
        const String &p_context, bool p_strict_mode);
Dictionary _layout_hint_build_snapshot(const GaussianStreamingSystem *p_system, bool p_strict_mode_enabled);
bool _validate_layout_hint_ranges(const Vector<GaussianStreamingSystem::ChunkLayoutHint> &p_hints,
        uint32_t p_index_space_count, bool p_use_source_offsets, bool p_require_remapped,
        bool p_forbid_remapped, bool p_allow_oversized_hints, LayoutHintValidationFailure &r_failure,
        uint64_t &r_total_hint_count, uint64_t &r_required_chunk_count, bool &r_saw_oversized_hint);

} // namespace gs_layout_hint

#endif // STREAMING_LAYOUT_HINT_H
