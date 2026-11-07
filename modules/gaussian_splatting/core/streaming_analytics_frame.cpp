#include "streaming_analytics_frame.h"

using AK = StreamingAnalyticsFrame::AnalyticsKey;
using DK = StreamingAnalyticsFrame::DiagnosticsKey;

#define GS_KEY_NAME(m_key) #m_key,
static const char *const ANALYTICS_KEY_NAMES[] = { GS_STREAMING_ANALYTICS_KEYS(GS_KEY_NAME) };
static const char *const DIAGNOSTICS_KEY_NAMES[] = { GS_STREAMING_DIAGNOSTICS_KEYS(GS_KEY_NAME) };
#undef GS_KEY_NAME

Dictionary StreamingAnalyticsFrame::_build_layout_hint_validation() const {
    return gs_layout_hint::_layout_hint_build_snapshot_from_counters(
            layout_hint_counters_valid ? &layout_hint_counters : nullptr, layout_hint_strict_mode);
}

Dictionary StreamingAnalyticsFrame::_build_diagnostics() {
    const Dictionary layout_hint_validation = _build_layout_hint_validation();
    diagnostics[DK::layout_hint_validation] = layout_hint_validation;
    diagnostics[DK::layout_hint_strict_mode_enabled] = layout_hint_strict_mode;
    diagnostics[DK::layout_hint_fallback_total] = layout_hint_validation.get("fallback_total", int64_t(0));
    diagnostics[DK::layout_hint_fallback_io_total] = layout_hint_validation.get("fallback_io_total", int64_t(0));
    diagnostics[DK::layout_hint_fallback_primary_total] = layout_hint_validation.get("fallback_primary_total", int64_t(0));
    diagnostics[DK::layout_hint_strict_failure_total] = layout_hint_validation.get("strict_failure_total", int64_t(0));
    diagnostics[DK::layout_hint_last_reason] = layout_hint_validation.get("last_reason", String("none"));
    diagnostics[DK::layout_hint_last_reason_category] = layout_hint_validation.get("last_reason_category", String("other"));
    diagnostics[DK::layout_hint_last_context] = layout_hint_validation.get("last_context", String("none"));
    diagnostics[DK::layout_hint_last_usage] = layout_hint_validation.get("last_usage", String("none"));

    Dictionary out;
    diagnostics.append_range(out, DIAGNOSTICS_KEY_NAMES, static_cast<DK>(0), DK::scheduler_visible_scan_budget_effective);
    StreamingTelemetryAdapter::apply_queue_pressure_diagnostics(out, diagnostics_queue_pressure);
    diagnostics.append_range(out, DIAGNOSTICS_KEY_NAMES, DK::scheduler_visible_scan_budget_effective, DK::COUNT);
    return out;
}

Dictionary StreamingAnalyticsFrame::resolve() {
    MutexLock lock(resolve_mutex);
    if (resolved) {
        return resolved_dictionary;
    }

    Dictionary latency_histograms;
    latency_histograms["bucket_upper_usec"] = StreamingLatencyHistogram::bucket_upper_bounds_usec();
    latency_histograms["read"] = StreamingLatencyHistogram::snapshot_to_dictionary(read_latency);
    latency_histograms["decode"] = StreamingLatencyHistogram::snapshot_to_dictionary(decode_latency);
    latency_histograms["pack"] = StreamingLatencyHistogram::snapshot_to_dictionary(pack_latency);
    latency_histograms["upload"] = StreamingLatencyHistogram::snapshot_to_dictionary(upload_latency);
    analytics[AK::streaming_latency_histograms] = latency_histograms;

    const Dictionary layout_hint_validation = _build_layout_hint_validation();
    analytics[AK::layout_hint_validation] = layout_hint_validation;
    analytics[AK::layout_hint_strict_mode_enabled] = layout_hint_strict_mode;
    analytics[AK::layout_hint_fallback_total] = layout_hint_validation.get("fallback_total", int64_t(0));
    analytics[AK::layout_hint_fallback_io_total] = layout_hint_validation.get("fallback_io_total", int64_t(0));
    analytics[AK::layout_hint_fallback_primary_total] = layout_hint_validation.get("fallback_primary_total", int64_t(0));
    analytics[AK::layout_hint_strict_failure_total] = layout_hint_validation.get("strict_failure_total", int64_t(0));
    analytics[AK::layout_hint_last_reason] = layout_hint_validation.get("last_reason", String("none"));
    analytics[AK::layout_hint_last_reason_category] = layout_hint_validation.get("last_reason_category", String("other"));
    analytics[AK::layout_hint_last_context] = layout_hint_validation.get("last_context", String("none"));
    analytics[AK::layout_hint_last_usage] = layout_hint_validation.get("last_usage", String("none"));

    Dictionary cap_sources;
    cap_sources["max_upload_mb_per_frame"] = analytics.get(AK::cap_source_upload_mb_per_frame, Variant());
    cap_sources["max_upload_mb_per_slice"] = analytics.get(AK::cap_source_upload_mb_per_slice, Variant());
    cap_sources["max_upload_mb_per_second"] = analytics.get(AK::cap_source_upload_mb_per_second, Variant());
    cap_sources["vram_budget_mb"] = analytics.get(AK::cap_source_vram_budget_mb, String("project_default"));
    cap_sources["min_chunks_in_vram"] = analytics.get(AK::cap_source_vram_min_chunks, String("project_default"));
    cap_sources["max_chunks_in_vram"] = analytics.get(AK::cap_source_vram_max_chunks, String("project_default"));
    analytics[AK::cap_sources] = cap_sources;

    const Dictionary streaming_diagnostics = _build_diagnostics();
    analytics[AK::diagnostics] = streaming_diagnostics;
    analytics[AK::diagnostics_category] = streaming_diagnostics.get("category", "ok");
    analytics[AK::diagnostics_reason] = streaming_diagnostics.get("reason", "healthy");
    analytics[AK::diagnostics_fingerprint] = streaming_diagnostics.get("fingerprint", "ok");
    analytics[AK::diagnostics_has_failure] = streaming_diagnostics.get("has_failure", false);

    Dictionary out = task_debug_state;
    analytics.append_range(out, ANALYTICS_KEY_NAMES, static_cast<AK>(0), AK::cap_sources);
    StreamingTelemetryAdapter::apply_queue_pressure_analytics(out, analytics_queue_pressure);
    analytics.append_range(out, ANALYTICS_KEY_NAMES, AK::cap_sources, AK::COUNT);
    if (overlay) {
        overlay(out);
    }

    resolved_dictionary = out;
    resolved = true;
    return resolved_dictionary;
}
