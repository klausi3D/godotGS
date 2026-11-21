extern "C" int test_hlod_wind_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../renderer/gs_wind_bounds.h"

TEST_CASE("[GaussianSplatting][HLOD] W-S frame displacement bound covers forced rows and additive effectors") {
    CHECK(gs_wind::displacement_bound(2.0, 3.0, 0.5) == doctest::Approx(9.0));
    CHECK(gs_wind::displacement_bound(2.0, 0.0, 5.0) == 0.0);
    CHECK(gs_wind::displacement_bound(0.0, 1.0, 0.0) == 0.0);
    InstanceDataGPU rows[2] = {};
    rows[0].params[2] = 5.0f;
    rows[0].params[3] = float(GS_INSTANCE_WIND_MODE_FORCE_DISABLED);
    rows[1].params[2] = 3.0f;
    rows[1].params[3] = float(GS_INSTANCE_WIND_MODE_INHERIT);
    gs::settings::GSWindSettings settings;
    settings.enabled = true;
    settings.strength = 2.0f;
    CHECK(gs_wind::frame_displacement_bound(settings, rows, 2, 0.0) == doctest::Approx(6.0));
    settings.enabled = false;
    CHECK(gs_wind::frame_displacement_bound(settings, rows, 2, 0.0) == 0.0);
    rows[1].params[3] = float(GS_INSTANCE_WIND_MODE_FORCE_ENABLED);
    settings.strength = 0.0f;
    CHECK(gs_wind::frame_displacement_bound(settings, rows, 2, 0.0) == doctest::Approx(3.0));
}

TEST_CASE("[GaussianSplatting][HLOD] wind active on a casting row query follows the shader enable and amplitude gates") {
    gs::settings::GSWindSettings settings;
    settings.enabled = true;
    settings.strength = 0.5f;
    InstanceDataGPU row = {};
    row.params[2] = 1.0f;
    CHECK(gs_wind::wind_active_on_casting_row(settings, row, true));
    CHECK_FALSE(gs_wind::wind_active_on_casting_row(settings, row, false));
    row.params[3] = float(GS_INSTANCE_WIND_MODE_FORCE_DISABLED);
    CHECK_FALSE(gs_wind::wind_active_on_casting_row(settings, row, true));
    settings.enabled = false;
    settings.strength = 0.0f;
    row.params[3] = float(GS_INSTANCE_WIND_MODE_FORCE_ENABLED);
    CHECK(gs_wind::wind_active_on_casting_row(settings, row, true)); // Shader's unit-strength fallback.
    row.params[2] = 0.0f;
    CHECK_FALSE(gs_wind::wind_active_on_casting_row(settings, row, true));
    row.params[2] = 1.0f;
    settings.direction = Vector3();
    CHECK_FALSE(gs_wind::wind_active_on_casting_row(settings, row, true));
    row.wind_params[1] = 1.0f;
    CHECK(gs_wind::wind_active_on_casting_row(settings, row, true)); // Row direction override.
}
