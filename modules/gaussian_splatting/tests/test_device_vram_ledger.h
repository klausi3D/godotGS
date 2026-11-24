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

// BV-17 (ADR adr-beyond-vram-worlds.md D-3): the atlas of a STREAMED HLOD world is a share of the ledger limit, not the 2 GiB
// staging ceiling of a one-shot upload, bounded by one atlas buffer (kMaxAtlasBufferBytes). The reserve below is the default route's: max_overlap_records 100M at the 32 B growth
// charge, and a 4M-splat sort at 12 B (half of the 24 B overlap record with 64-bit keys).
namespace DeviceVramLedgerTests {
static constexpr uint64_t kGiB = uint64_t(1) << 30;
static constexpr uint64_t kStride = 176; // sizeof(PackedGaussian)
static constexpr uint64_t kPageSplats = 1024;
static uint64_t default_reserve() {
    return ResidentAtlasBudget::hlod_streamed_frame_reserve_bytes(100000000u, 32u, 4000000u, 12u);
}
static uint64_t pages_for(uint64_t p_atlas_bytes) {
    const uint64_t huge = uint64_t(1) << 50;
    return ResidentAtlasBudget::hlod_stream_page_budget(p_atlas_bytes, kStride, kPageSplats, huge, 0, huge, huge, 0, 0, 0);
}
} // namespace DeviceVramLedgerTests

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger][HLOD] a 24 GiB device streams more than the 2 GiB staging ceiling; the atlas buffer binds (BV-17)") {
    using namespace DeviceVramLedgerTests;
    using Bound = DeviceVramLedger::StreamedAtlasBound;
    const uint64_t heap = 24 * kGiB; // RTX 3090
    const uint64_t limit = DeviceVramLedger::gs_limit_for_device(heap, kCap);
    CHECK(limit == uint64_t(double(heap) * 0.60));
    // The resident and whole-tree atlas keeps the staging ceiling (one-shot upload): unchanged.
    CHECK(DeviceVramLedger::atlas_limit_for_device(heap, 0) == DeviceVramLedger::kResidentStagingCeilingBytes);
    // The streamed atlas: 14.4 GiB of ledger less 3.25 GB of frame reserve is more than five atlas buffers, so the one
    // buffer binds, and it is named as what binds (not the staging ceiling, and not the ledger).
    const uint64_t share = limit - default_reserve();
    CHECK(share > 2 * DeviceVramLedger::kMaxAtlasBufferBytes);
    const DeviceVramLedger::StreamedAtlasLimit streamed = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 0, default_reserve());
    CHECK(streamed.bytes > DeviceVramLedger::kResidentStagingCeilingBytes);
    CHECK(streamed.bytes == DeviceVramLedger::kMaxAtlasBufferBytes);
    CHECK(streamed.bound == Bound::BUFFER_LIMIT);
    CHECK(String(DeviceVramLedger::streamed_atlas_bound_name(streamed.bound)) == "buffer_limit");
    // In 176 B records: 23,831 pages (24.4M record slots) where the staging ceiling allowed 11,915.
    CHECK(pages_for(DeviceVramLedger::kResidentStagingCeilingBytes) == 11915);
    CHECK(pages_for(streamed.bytes) == 23831);
    CHECK(pages_for(streamed.bytes) * kPageSplats * kStride <= DeviceVramLedger::kMaxAtlasBufferBytes);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger][HLOD] an 8 GiB device streams the ledger share left after the frame reserve (BV-17)") {
    using namespace DeviceVramLedgerTests;
    using Bound = DeviceVramLedger::StreamedAtlasBound;
    const uint64_t heap = 8 * kGiB;
    const uint64_t limit = DeviceVramLedger::gs_limit_for_device(heap, kCap);
    const uint64_t reserve = default_reserve();
    CHECK(reserve == uint64_t(100000000) * 32 + uint64_t(4000000) * 12);
    const DeviceVramLedger::StreamedAtlasLimit streamed = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 0, reserve);
    CHECK(streamed.bound == Bound::LEDGER_SHARE);
    // Exactly the share: the pool and the frame reserve together fill the ledger limit, never more.
    CHECK(streamed.bytes + reserve == limit);
    CHECK(streamed.bytes == uint64_t(double(heap) * 0.60) - reserve);
    // On this device the overlap storage at its cap leaves less than the staging ceiling: the share is the honest bound.
    CHECK(streamed.bytes < DeviceVramLedger::kResidentStagingCeilingBytes);
    // A smaller overlap cap (a lower quality tier) leaves the pool more, through the same accounting.
    const uint64_t low_reserve = ResidentAtlasBudget::hlod_streamed_frame_reserve_bytes(30000000u, 32u, 4000000u, 12u);
    const DeviceVramLedger::StreamedAtlasLimit low = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 0, low_reserve);
    CHECK(low.bound == Bound::LEDGER_SHARE);
    CHECK(low.bytes == limit - low_reserve);
    CHECK(low.bytes > DeviceVramLedger::kResidentStagingCeilingBytes);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger][HLOD] the streamed atlas clamps at one storage buffer, exactly at the bound (BV-17)") {
    using namespace DeviceVramLedgerTests;
    using Bound = DeviceVramLedger::StreamedAtlasBound;
    const uint64_t heap = 64 * kGiB;
    const uint64_t buffer = 1000 * kMiB;
    // Share == buffer: not clamped. Share == buffer + 1: clamped to the buffer and named.
    DeviceVramLedger::StreamedAtlasLimit at = DeviceVramLedger::streamed_atlas_limit_for_device(heap, buffer + 77, 0, 77, buffer);
    CHECK(at.bytes == buffer);
    CHECK(at.bound == Bound::LEDGER_SHARE);
    at = DeviceVramLedger::streamed_atlas_limit_for_device(heap, buffer + 78, 0, 77, buffer);
    CHECK(at.bytes == buffer);
    CHECK(at.bound == Bound::BUFFER_LIMIT);
    // An override above the buffer is clamped too: never a buffer above the 32-bit size hlod_create_buffer accepts.
    const DeviceVramLedger::StreamedAtlasLimit big = DeviceVramLedger::streamed_atlas_limit_for_device(heap, 40 * kGiB, 8192, 0);
    CHECK(big.bytes == DeviceVramLedger::kMaxAtlasBufferBytes);
    CHECK(big.bound == Bound::BUFFER_LIMIT);
    // The pages of the clamped limit fit the buffer and a 32-bit record count; one more page would not fit the buffer.
    const uint64_t pages = pages_for(big.bytes);
    CHECK(pages * kPageSplats * kStride <= DeviceVramLedger::kMaxAtlasBufferBytes);
    CHECK((pages + 1) * kPageSplats * kStride > DeviceVramLedger::kMaxAtlasBufferBytes);
    CHECK(pages * kPageSplats <= UINT32_MAX);
    // The default bound is the 32-bit buffer size hlod_create_buffer accepts.
    CHECK(DeviceVramLedger::kMaxAtlasBufferBytes == uint64_t(UINT32_MAX));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger][HLOD] the override pins the streamed atlas in both directions (BV-17)") {
    using namespace DeviceVramLedgerTests;
    using Bound = DeviceVramLedger::StreamedAtlasBound;
    const uint64_t reserve = default_reserve();
    // Down on a 24 GiB device: the forced-streaming lever (512 MiB) keeps working.
    const uint64_t big_heap = 24 * kGiB;
    DeviceVramLedger::StreamedAtlasLimit l = DeviceVramLedger::streamed_atlas_limit_for_device(big_heap,
            DeviceVramLedger::gs_limit_for_device(big_heap, kCap), 512, reserve);
    CHECK(l.bytes == 512 * kMiB);
    CHECK(l.bound == Bound::OVERRIDE);
    // Up on an 8 GiB device: above the derived share (1.78 GiB). The page budget's ledger term still bounds the pool by the
    // live device bytes.
    const uint64_t heap = 8 * kGiB;
    const uint64_t limit = DeviceVramLedger::gs_limit_for_device(heap, kCap);
    l = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 2000, reserve);
    CHECK(l.bytes == 2000 * kMiB);
    CHECK(l.bytes > limit - reserve);
    CHECK(l.bound == Bound::OVERRIDE);
    // Up past the atlas buffer: clamped and named.
    l = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 4096, reserve);
    CHECK(l.bytes == DeviceVramLedger::kMaxAtlasBufferBytes);
    CHECK(l.bound == Bound::BUFFER_LIMIT);
    // The whole-tree and resident atlas keep the MIN semantics: an override only lowers it.
    CHECK(DeviceVramLedger::atlas_limit_for_device(heap, 3072) == DeviceVramLedger::kResidentStagingCeilingBytes);
    CHECK(DeviceVramLedger::atlas_limit_for_device(heap, 512) == 512 * kMiB);
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][DeviceVramLedger][HLOD] the streamed atlas fails closed: unknown heap, no ledger share, unprovable reserve (BV-17)") {
    using namespace DeviceVramLedgerTests;
    using Bound = DeviceVramLedger::StreamedAtlasBound;
    // Unknown or untrusted heap: the staging ceiling, as before.
    for (uint64_t heap : { uint64_t(0), uint64_t(200) * kMiB }) {
        const DeviceVramLedger::StreamedAtlasLimit l = DeviceVramLedger::streamed_atlas_limit_for_device(heap,
                DeviceVramLedger::gs_limit_for_device(heap, kCap), 0, default_reserve());
        CHECK(l.bytes == DeviceVramLedger::kResidentStagingCeilingBytes);
        CHECK(l.bound == Bound::UNKNOWN_HEAP);
    }
    // The reserve alone fills the ledger (at and above the limit): no share, named, and no page is admissible.
    const uint64_t heap = 4 * kGiB;
    const uint64_t limit = DeviceVramLedger::gs_limit_for_device(heap, kCap);
    for (uint64_t reserve : { limit, limit + 1, uint64_t(UINT64_MAX) }) {
        const DeviceVramLedger::StreamedAtlasLimit l = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 0, reserve);
        CHECK(l.bytes == 0);
        CHECK(l.bound == Bound::NO_LEDGER_SHARE);
        CHECK(String(DeviceVramLedger::streamed_atlas_bound_name(l.bound)) == "no_ledger_share");
        CHECK(pages_for(l.bytes) == 0);
    }
    // One byte of share left: LEDGER_SHARE, but below one page, so still no page.
    const DeviceVramLedger::StreamedAtlasLimit one = DeviceVramLedger::streamed_atlas_limit_for_device(heap, limit, 0, limit - 1);
    CHECK(one.bytes == 1);
    CHECK(one.bound == Bound::LEDGER_SHARE);
    CHECK(pages_for(one.bytes) == 0);
    // The reserve saturates instead of wrapping.
    CHECK(ResidentAtlasBudget::hlod_streamed_frame_reserve_bytes(UINT64_MAX / 2, 32u, 0, 0) == UINT64_MAX);
    CHECK(ResidentAtlasBudget::hlod_streamed_frame_reserve_bytes(UINT64_MAX / 32, 32u, UINT64_MAX / 12, 12u) == UINT64_MAX);
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
