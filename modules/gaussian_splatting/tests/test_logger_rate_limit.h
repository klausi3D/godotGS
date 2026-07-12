#pragma once

#include "test_macros.h"
#include "../logger/gs_logger.h"

TEST_CASE("[Gaussian Logger] Rate limiter keys low-severity logs by category level and fingerprint") {
    using namespace gs_logger;

    test::reset_rate_limiter();

    const uint64_t t0 = 1'000'000;
    const uint64_t window = 500'000;
    const String message = "shared-fingerprint";

    CHECK(test::check_rate_limit(Category::STREAMING, Level::INFO, message, t0, window));
    CHECK_FALSE(test::check_rate_limit(Category::STREAMING, Level::INFO, message, t0 + 1000, window));

    // WARN/ERROR are a separate severity path and should not be blocked by INFO chatter.
    CHECK(test::check_rate_limit(Category::STREAMING, Level::WARN, message, t0 + 2000, window));
    CHECK(test::check_rate_limit(Category::STREAMING, Level::ERROR, message, t0 + 3000, window));

    // Different message fingerprint should not be suppressed either.
    CHECK(test::check_rate_limit(Category::STREAMING, Level::INFO, "different-fingerprint", t0 + 4000, window));
}

TEST_CASE("[Gaussian Logger] Rate limiter does not suppress high-severity repeats") {
    using namespace gs_logger;

    test::reset_rate_limiter();

    const uint64_t t0 = 2'000'000;
    const uint64_t window = 500'000;
    const String message = "high-severity";

    CHECK(test::check_rate_limit(Category::RENDERER, Level::WARN, message, t0, window));
    CHECK(test::check_rate_limit(Category::RENDERER, Level::WARN, message, t0 + 1000, window));
    CHECK(test::check_rate_limit(Category::RENDERER, Level::ERROR, message, t0 + 2000, window));
    CHECK(test::check_rate_limit(Category::RENDERER, Level::ERROR, message, t0 + 3000, window));
}

// --- Truthful log-level semantics (#172) ---------------------------------
// These exercise the pure effective-level logic that is_enabled() also uses.
// Levels are ordered off < error < warn < info < debug < trace (off = quietest).

TEST_CASE("[Gaussian Logger] Default categories inherit and resolve to WARN (output-neutral)") {
    using namespace gs_logger;
    // Every category defaults to INHERIT and the master verbosity defaults to WARN,
    // so the effective ceiling is WARN -- identical to the historical
    // min(WARN, WARN) == WARN behavior.
    const Level effective = resolve_effective_level(Level::INHERIT, Level::WARN);
    CHECK(effective == Level::WARN);
    CHECK(level_permits(Level::ERROR, effective));
    CHECK(level_permits(Level::WARN, effective));
    CHECK_FALSE(level_permits(Level::INFO, effective));
    CHECK_FALSE(level_permits(Level::DEBUG, effective));
    CHECK_FALSE(level_permits(Level::TRACE, effective));
}

TEST_CASE("[Gaussian Logger] verbosity is the master level for inherit categories") {
    using namespace gs_logger;
    // Raising verbosity to INFO (categories still inherit) makes INFO visible for
    // every category. Impossible under the old min(category, verbosity) rule, which
    // capped inherit categories at WARN regardless of verbosity.
    const Level effective = resolve_effective_level(Level::INHERIT, Level::INFO);
    CHECK(effective == Level::INFO);
    CHECK(level_permits(Level::INFO, effective));
    CHECK(level_permits(Level::WARN, effective));
    CHECK_FALSE(level_permits(Level::DEBUG, effective));
}

TEST_CASE("[Gaussian Logger] An explicit category level overrides the master verbosity") {
    using namespace gs_logger;
    // renderer=DEBUG while verbosity=WARN must show renderer DEBUG (old rule capped
    // it at WARN). A different category left at INHERIT still follows verbosity.
    const Level renderer_effective = resolve_effective_level(Level::DEBUG, Level::WARN);
    CHECK(renderer_effective == Level::DEBUG);
    CHECK(level_permits(Level::DEBUG, renderer_effective));

    const Level other_effective = resolve_effective_level(Level::INHERIT, Level::WARN);
    CHECK(other_effective == Level::WARN);
    CHECK_FALSE(level_permits(Level::DEBUG, other_effective));
}

TEST_CASE("[Gaussian Logger] silent is a truthful OFF alias that suppresses everything") {
    using namespace gs_logger;
    // "silent" now parses to OFF (not WARN). verbosity OFF + inherit categories
    // suppresses even errors and warnings.
    CHECK(test::parse_level("silent", Level::WARN) == Level::OFF);
    CHECK(test::parse_level("off", Level::WARN) == Level::OFF);
    const Level effective = resolve_effective_level(Level::INHERIT, Level::OFF);
    CHECK(effective == Level::OFF);
    CHECK_FALSE(level_permits(Level::ERROR, effective));
    CHECK_FALSE(level_permits(Level::WARN, effective));
}

TEST_CASE("[Gaussian Logger] inherit sentinel follows verbosity across levels") {
    using namespace gs_logger;
    CHECK(test::parse_level("inherit", Level::WARN) == Level::INHERIT);
    // A category at INHERIT tracks whatever verbosity is set to...
    CHECK(resolve_effective_level(Level::INHERIT, Level::OFF) == Level::OFF);
    CHECK(resolve_effective_level(Level::INHERIT, Level::WARN) == Level::WARN);
    CHECK(resolve_effective_level(Level::INHERIT, Level::TRACE) == Level::TRACE);
    // ...while an explicit level ignores verbosity entirely.
    CHECK(resolve_effective_level(Level::ERROR, Level::TRACE) == Level::ERROR);
}

TEST_CASE("[Gaussian Logger] An explicit category level overrides a global OFF verbosity") {
    using namespace gs_logger;
    // Regression (#172): verbosity=OFF is a truthful global "silent". An INHERIT
    // category under it stays fully suppressed, but an EXPLICIT category level still
    // wins -- the old min(category, OFF) == OFF rule would have killed it. This is
    // the "explicit override beats even a global kill" boundary, the one case where
    // the new semantics visibly diverge from the historical min() behavior.
    const Level explicit_debug = resolve_effective_level(Level::DEBUG, Level::OFF);
    CHECK(explicit_debug == Level::DEBUG);
    CHECK(level_permits(Level::ERROR, explicit_debug));
    CHECK(level_permits(Level::WARN, explicit_debug));
    CHECK(level_permits(Level::INFO, explicit_debug));
    CHECK(level_permits(Level::DEBUG, explicit_debug));
    CHECK_FALSE(level_permits(Level::TRACE, explicit_debug));

    // Contrast: an INHERIT category under the same global OFF resolves to OFF and
    // suppresses everything, so OFF remains a hard kill for inheriting categories.
    const Level inherited = resolve_effective_level(Level::INHERIT, Level::OFF);
    CHECK(inherited == Level::OFF);
    CHECK_FALSE(level_permits(Level::DEBUG, inherited));
    CHECK_FALSE(level_permits(Level::ERROR, inherited));
}

TEST_CASE("[Gaussian Logger] Numeric per-category override follows the category enum hint") {
    using namespace gs_logger;
    // A numeric per-category override must be read as an index into the category
    // enum hint "inherit,off,error,warn,info,debug,trace" (index 0 == sentinel),
    // NOT cast straight to the Level enum value (where 0 == OFF). The old code cast
    // directly, so index 3 ("warn" in the hint) wrongly became Level::INFO.
    CHECK(test::category_level_from_enum_index(0) == Level::INHERIT);
    CHECK(test::category_level_from_enum_index(1) == Level::OFF);
    CHECK(test::category_level_from_enum_index(2) == Level::ERROR);
    CHECK(test::category_level_from_enum_index(3) == Level::WARN);
    CHECK(test::category_level_from_enum_index(4) == Level::INFO);
    CHECK(test::category_level_from_enum_index(5) == Level::DEBUG);
    CHECK(test::category_level_from_enum_index(6) == Level::TRACE);
    // The numeric index and its hint label must agree: index 3 is "warn", so it
    // resolves to exactly what parse_level("warn") does.
    CHECK(test::category_level_from_enum_index(3) == test::parse_level("warn", Level::OFF));
    CHECK(test::category_level_from_enum_index(0) == test::parse_level("inherit", Level::OFF));
    // Out-of-range indices clamp to the hint bounds (inherit .. trace).
    CHECK(test::category_level_from_enum_index(-5) == Level::INHERIT);
    CHECK(test::category_level_from_enum_index(99) == Level::TRACE);
}
