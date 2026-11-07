#pragma once

#include "test_macros.h"

// [VRAMBudgetRegulator] is the strict lane of run_module_tests.py: a case only under an unlisted tag lands in the advisory
// [untagged] net, where a failure cannot fail CI.

#include "../renderer/device_vram_ledger.h"
#include "../renderer/resident_atlas_budget.h"

namespace DeviceVramLedgerTests {

static constexpr uint64_t kMiB = uint64_t(1) << 20;
static constexpr uint64_t kCap = 512 * kMiB;

// A ledger with a known limit of 1000 B: the heap is below the trust floor, so the limit is min(heap, fixed cap).
static void configure_1000(DeviceVramLedger &r_ledger) {
    r_ledger.configure(1000, 1000);
    REQUIRE(r_ledger.limit() == 1000);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger] limits are the published 0.60 share, floor and fallback") {
    // 3080: 10 GiB of heap. 6 GiB of GS share, 2 GiB of atlas (the staging ceiling binds), 4 GiB of reserve.
    DeviceVramLedger ledger;
    const uint64_t heap = 10240 * kMiB;
    ledger.configure(heap, kCap);
    CHECK(ledger.limit() == uint64_t(double(heap) * 0.60));
    CHECK(ledger.atlas_limit(0) == uint64_t(2048) * kMiB);
    CHECK(ledger.atlas_limit(100) == 100 * kMiB);
    CHECK(ledger.snapshot().engine_driver_reserve_bytes == heap - ledger.limit());

    // Unknown and untrusted heaps keep the fixed cap and the staging ceiling.
    CHECK(DeviceVramLedger::gs_limit_for_device(0, kCap) == kCap);
    CHECK(DeviceVramLedger::gs_limit_for_device(200 * kMiB, 100 * kMiB) == 100 * kMiB);
    CHECK(DeviceVramLedger::gs_limit_for_device(200 * kMiB, kCap) == 200 * kMiB);
    CHECK(DeviceVramLedger::atlas_limit_for_device(200 * kMiB, 0) == uint64_t(2048) * kMiB);

    // The frame route's test clamp only ever lowers the limit.
    ledger.configure(heap, kCap, 1000);
    CHECK(ledger.limit() == 1000);
    ledger.configure(heap, kCap, UINT64_MAX);
    CHECK(ledger.limit() == uint64_t(double(heap) * 0.60));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger] the publication decision equals hlod_candidate_fits at the boundary") {
    for (uint64_t device_bytes : { uint64_t(0), uint64_t(899), uint64_t(900), uint64_t(901), uint64_t(1000), uint64_t(1001) }) {
        for (uint64_t candidate : { uint64_t(0), uint64_t(99), uint64_t(100), uint64_t(101), uint64_t(1001) }) {
            DeviceVramLedger ledger;
            configure_1000(ledger);
            ledger.observe_device_bytes(device_bytes);
            CHECK(ledger.reserve(DeviceVramLedger::POOL, candidate) ==
                    ResidentAtlasBudget::hlod_candidate_fits(candidate, device_bytes, 1000, UINT64_MAX));
        }
    }
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger] nothing is counted twice") {
    DeviceVramLedger ledger;
    configure_1000(ledger);

    // The same memory reported again replaces itself.
    ledger.report(DeviceVramLedger::SORT, 100);
    ledger.report(DeviceVramLedger::SORT, 100);
    CHECK(ledger.headroom() == 900);
    CHECK(ledger.snapshot().attributed_bytes == 100);

    // A retried reservation replaces itself.
    CHECK(ledger.reserve(DeviceVramLedger::POOL, 300));
    CHECK(ledger.reserve(DeviceVramLedger::POOL, 300));
    CHECK(ledger.headroom() == 600);
    CHECK(ledger.snapshot().pending_bytes == 300);

    // The measurement already holds the live categories: they explain it, they do not add to it.
    ledger.observe_device_bytes(250);
    CHECK(ledger.headroom() == 450); // 1000 - 250 measured - 300 pending, not 350.
    const DeviceVramLedger::Snapshot before = ledger.snapshot();
    CHECK(before.attributed_bytes == 100);
    CHECK(before.unattributed_bytes == 150);

    // The reserved allocation lands: it moves from pending into the next measurement, it is not held in both.
    ledger.release(DeviceVramLedger::POOL);
    ledger.observe_device_bytes(550);
    ledger.report(DeviceVramLedger::POOL, 300);
    CHECK(ledger.headroom() == 450);
    CHECK(ledger.snapshot().unattributed_bytes == 150);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger] a release gives the bytes back on every path") {
    DeviceVramLedger ledger;
    configure_1000(ledger);
    ledger.observe_device_bytes(400);
    {
        DeviceVramLedger::ScopedReservation held(ledger, DeviceVramLedger::POOL);
        CHECK(held.acquire(500));
        CHECK(ledger.headroom() == 100);
        // Other categories see the reservation.
        CHECK_FALSE(ledger.reserve(DeviceVramLedger::OVERLAP, 101));
        CHECK(ledger.snapshot().refused_reservations == 1);
        // A refused growth keeps what was held.
        CHECK_FALSE(held.acquire(601));
        CHECK(ledger.headroom() == 100);
    }
    CHECK(ledger.headroom() == 600);
    CHECK(ledger.snapshot().pending_bytes == 0);
    {
        // A refused first reservation holds nothing and must not release someone else's bytes.
        CHECK(ledger.reserve(DeviceVramLedger::POOL, 100));
        DeviceVramLedger::ScopedReservation refused(ledger, DeviceVramLedger::OVERLAP);
        CHECK_FALSE(refused.acquire(501));
    }
    CHECK(ledger.headroom() == 500);
    ledger.release(DeviceVramLedger::POOL);

    // A shrinking measurement gives bytes back too, and an exceeded limit has no headroom at all.
    ledger.observe_device_bytes(1200);
    CHECK(ledger.headroom() == 0);
    ledger.observe_device_bytes(0);
    CHECK(ledger.headroom() == 1000);

    // Released HLOD bundles leave no stale category behind; the chunk route keeps its report.
    ledger.report(DeviceVramLedger::POOL, 100);
    ledger.report(DeviceVramLedger::CHUNK_POOL, 50);
    ledger.clear_hlod_reports();
    CHECK(ledger.snapshot().live[DeviceVramLedger::POOL] == 0);
    CHECK(ledger.snapshot().attributed_bytes == 50);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger] enforced on the HLOD route, report-only on the chunk route, fail closed") {
    DeviceVramLedger unconfigured;
    CHECK_FALSE(unconfigured.reserve(DeviceVramLedger::POOL, 1));
    unconfigured.report(DeviceVramLedger::CHUNK_POOL, 1000);
    CHECK(unconfigured.snapshot().report_only_overruns == 0); // No limit known: nothing to overrun.

    DeviceVramLedger ledger;
    configure_1000(ledger);
    ledger.observe_device_bytes(950);
    CHECK_FALSE(ledger.reserve(DeviceVramLedger::POOL, 100));
    CHECK(ledger.snapshot().report_only_overruns == 0);

    // The chunk route is never refused, and every overrun is visible.
    CHECK(ledger.reserve(DeviceVramLedger::CHUNK_POOL, 100));
    CHECK(ledger.snapshot().report_only_overruns == 1);
    ledger.release(DeviceVramLedger::CHUNK_POOL);
    ledger.report(DeviceVramLedger::CHUNK_POOL, 900);
    CHECK(ledger.snapshot().report_only_overruns == 1); // 950 measured <= 1000.
    ledger.observe_device_bytes(1001);
    ledger.report(DeviceVramLedger::CHUNK_POOL, 900);
    CHECK(ledger.snapshot().report_only_overruns == 2);
}

} // namespace DeviceVramLedgerTests
