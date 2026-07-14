#ifndef PIPELINE_FEATURE_SET_H
#define PIPELINE_FEATURE_SET_H

#include <cstdint>

#include "core/config/project_settings.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

class RenderingDevice;

struct PipelineFeatureSet {
    // Packed payloads store the projected global index in 16 bits.
    static constexpr uint32_t PACKED_STAGE_MAX_TOTAL_SPLATS = 65535u;

    bool enable_packed_stage_data = false;
    bool enable_tighter_bounds = false;
    bool enable_fast_raster = false;
    bool enable_sh_amortization = false;
    // Force-enables exactly the four pipeline experimental toggles above; the
    // scope is the pipeline feature set, not the engine (#169).
    bool enable_all_pipeline_experimental = false;
    int sh_amortization_divisor = 10;

    static const String SECTION_PATH;
    static const String ENABLE_PACKED_STAGE_DATA_PATH;
    static const String ENABLE_TIGHTER_BOUNDS_PATH;
    static const String ENABLE_FAST_RASTER_PATH;
    static const String ENABLE_SH_AMORTIZATION_PATH;
    static const String SH_AMORTIZATION_DIVISOR_PATH;
    // Canonical key (#169): pipeline/enable_all_pipeline_experimental. The old
    // pipeline/enable_all_experimental spelling is honored as a read-only
    // deprecated alias (see pipeline_feature_set.cpp).
    static const String ENABLE_ALL_PIPELINE_EXPERIMENTAL_PATH;
    static const String DEPRECATED_ENABLE_ALL_EXPERIMENTAL_PATH;

    void load_from_project_settings();
    void save_to_project_settings() const;
    void reset_to_defaults();

    bool validate(uint32_t p_total_gaussians = 0) const;
    String get_validation_errors(uint32_t p_total_gaussians = 0) const;

    PipelineFeatureSet get_effective(RenderingDevice *p_device,
            bool p_compute_raster_enabled,
            bool p_global_sort_enabled,
            String *r_warnings = nullptr) const;

    Dictionary get_effective_config_snapshot() const;

    void print_config_summary() const;

    Dictionary loaded_provenance_snapshot;
    mutable Dictionary effective_provenance_snapshot;
    mutable bool effective_provenance_snapshot_valid = false;

    // Releases provenance snapshot Dictionaries, dropping the StringName
    // keys that constructor SNAME-style helpers cache for the lifetime of
    // the snapshot. Called at module unregister so the engine's exit-time
    // orphan StringName report stays bounded. See:
    // tests/ci/run_module_tests.py::_run_stringname_orphan_guard_step
    void clear_provenance_snapshots();
};

extern PipelineFeatureSet g_pipeline_feature_set;

void initialize_pipeline_feature_set();

// Releases the global pipeline feature set's provenance snapshots. Wrapper
// for register_types.cpp::uninitialize_gaussian_splatting_module() so the
// per-call-site StringName keys (pipeline_packed_stage_data, ..., value,
// source, ...) do not show up in the exit-time orphan StringName report.
void release_pipeline_feature_set_module_strings();

#endif // PIPELINE_FEATURE_SET_H
