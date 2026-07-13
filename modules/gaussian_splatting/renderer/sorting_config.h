#ifndef SORTING_CONFIG_H
#define SORTING_CONFIG_H

#include <cstdint>

#include "core/string/string_name.h"
#include "core/string/ustring.h"

class ProjectSettings;

struct SortingStrategyConfig {
    enum class ForcedAlgorithm : uint8_t {
        AUTO = 0,
        RADIX = 1,
        BITONIC = 2,
        ONESWEEP = 3
    };

    // Live AUTO band boundaries consumed by GPUSorterFactory::AutoThresholds:
    //   bitonic_max_elements = the bitonic->radix boundary (count <= it -> BITONIC)
    //   radix_max_elements   = the radix->onesweep boundary (count >= it -> ONESWEEP)
    // Defaults reproduce the historical hardcoded AUTO thresholds
    // (32768 / 1048576) so an unconfigured project selects the same algorithm as
    // before (#168). The OneSweep band is unbounded above, so there is no third
    // boundary to configure.
    uint32_t bitonic_max_elements = 32768;
    uint32_t radix_max_elements = 1048576;
    uint32_t history_size = 120;
    uint32_t log_interval_frames = 60;
    float target_sort_time_ms = 2.0f;
    bool log_metrics = true;
    ForcedAlgorithm force_algorithm = ForcedAlgorithm::AUTO;
    bool force_cpu_sort = false;

    void sanitize();
    String describe_thresholds() const;
    bool is_algorithm_forced() const;
    String get_forced_algorithm_name() const;

    static SortingStrategyConfig load_from_project_settings();
};

#endif // SORTING_CONFIG_H
