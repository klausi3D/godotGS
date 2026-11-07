#ifndef GS_DEVICE_VRAM_LEDGER_H
#define GS_DEVICE_VRAM_LEDGER_H

// The one device VRAM ledger (ADR adr-beyond-vram-worlds.md D-11, BV-11). It owns the GS share of the device heap, the
// atlas limit derived from it, the device-wide byte measurement the HLOD route is admitted against, and the split of
// that measurement by category. Everything else reports through it. The class is pure (no RenderingDevice, no project
// settings) so that the admission math is testable on the host; renderer/resident_instance_contract_publisher.cpp feeds it.
//
// How bytes are counted, so that nothing is counted twice:
//  - observe_device_bytes() is the measured device-wide upper bound (every RD buffer plus the tracked textures). It is
//    the admission basis. The categories only EXPLAIN it: report() replaces a category's live bytes, it never adds, and
//    live bytes are never summed on top of the measurement. What no category explains is `unattributed_bytes`.
//  - Without a measurement (nothing observed yet) the sum of the live categories is the basis.
//  - reserve() pre-flights bytes that are not allocated yet. It replaces the category's own reservation. The caller
//    releases it once the allocation exists or failed, and the next measurement carries the allocated bytes. While the
//    old allocation of a resize is still alive it stays in the measurement, so a reservation of the whole new size
//    pre-flights old + new (I-6).
//
// D-11: enforced on the HLOD route, report-only on the chunk route until Step C. CHUNK_POOL never refuses; going over the
// limit there only counts report_only_overruns. The engine and driver reserves are the rest of the heap (1 - 0.60): the
// repo has no measurement that splits them, so they are published as one number.
//
// Threads: the render thread is the only writer (I-11). The fields are relaxed atomics so that the diagnostics may read a
// snapshot from any thread; a snapshot is therefore per-field, not a consistent cut of one frame.

#include <atomic>
#include <cstdint>

class DeviceVramLedger {
public:
	enum Category : uint8_t {
		POOL = 0, // The world's atlas in the HLOD bundles (streamed pool or whole tree).
		SORT, // The shared sort keys and indices.
		OVERLAP, // The tile overlap sort storage.
		VIEW, // Per-visible and per-view buffers of the HLOD bundles (refs, draw records, rows, metadata).
		HLOD_SCRATCH, // Projection, subpixel history and visibility, SH colour cache.
		TARGET_TEXTURES, // Tile and compositor textures.
		OBJECT_ATLAS, // The resident object atlas.
		INSTANCE_REGION, // The node instances' region at the HLOD atlas tail.
		SHADOW_CASTER, // The shadow caster's own buffers.
		CHUNK_POOL, // The chunk route's streaming footprint. Report-only (D-11).
		CATEGORY_COUNT,
	};

	// The GS-owned share of the device heap (D-11). The remainder is the engine and driver reserve:
	// get_device_memory_budget() is the device heap, not net of anything.
	static constexpr double kGsDeviceShare = 0.60;
	// Below this floor a reported device heap is treated as unknown: a degenerate small-positive value would otherwise
	// clamp every budget to nonsense.
	static constexpr uint64_t kMinTrustedDeviceBytes = uint64_t(256) * 1024 * 1024;
	// Absolute single-upload staging ceiling of the resident atlas; a budget only ever tightens beneath it.
	static constexpr uint64_t kResidentStagingCeilingBytes = uint64_t(2) * 1024 * 1024 * 1024;

	// Byte cap of a packed resident atlas.
	//  - An unknown or untrusted heap keeps the staging ceiling (do-no-harm on UMA/iGPU/software/non-Vulkan).
	//  - override_mb > 0 pins an explicit cap (low-end clamp + on-capable-GPU validation).
	// The 0.60 term only binds below ~3.3 GB of device VRAM; above that the staging ceiling dominates.
	static uint64_t atlas_limit_for_device(uint64_t p_device_bytes, uint32_t p_override_mb,
			uint64_t p_staging_ceiling_bytes = kResidentStagingCeilingBytes, double p_share = kGsDeviceShare,
			uint64_t p_min_trusted_bytes = kMinTrustedDeviceBytes) {
		uint64_t cap = p_staging_ceiling_bytes;
		if (p_device_bytes >= p_min_trusted_bytes) {
			const uint64_t share_cap = uint64_t(double(p_device_bytes) * p_share);
			cap = share_cap < cap ? share_cap : cap;
		}
		if (p_override_mb > 0) {
			const uint64_t override_bytes = uint64_t(p_override_mb) * 1024u * 1024u;
			cap = override_bytes < cap ? override_bytes : cap;
		}
		return cap;
	}

	// The GS limit. It holds the canonical route's own sort, tile and atlas buffers too, so it cannot be bounded by the
	// HLOD-owned byte cap; that cap bounds the HLOD bundle on its own. An unknown or untrusted heap size keeps the fixed
	// cap (fail closed).
	static uint64_t gs_limit_for_device(uint64_t p_device_bytes, uint64_t p_fixed_cap_bytes, double p_share = kGsDeviceShare,
			uint64_t p_min_trusted_bytes = kMinTrustedDeviceBytes) {
		if (p_device_bytes >= p_min_trusted_bytes) {
			return uint64_t(double(p_device_bytes) * p_share);
		}
		if (p_device_bytes == 0) { return p_fixed_cap_bytes; }
		return p_device_bytes < p_fixed_cap_bytes ? p_device_bytes : p_fixed_cap_bytes;
	}

	static const char *category_name(Category p_category) {
		static const char *names[CATEGORY_COUNT] = { "pool", "sort", "overlap", "view", "hlod_scratch", "target_textures",
			"object_atlas", "instance_region", "shadow_caster", "chunk_pool" };
		return p_category < CATEGORY_COUNT ? names[p_category] : "invalid";
	}
	static bool is_enforced(Category p_category) { return p_category != CHUNK_POOL; }

	struct Snapshot {
		uint64_t device_heap_bytes = 0;
		uint64_t limit_bytes = 0; // The GS share (or the fallback cap on an unknown heap, or a test clamp).
		uint64_t engine_driver_reserve_bytes = 0; // device_heap_bytes - limit_bytes.
		bool device_observed = false;
		uint64_t device_bytes = 0; // The latest measurement.
		uint64_t admitted_device_bytes = 0; // The measurement of the last admitted HLOD frame.
		uint64_t attributed_bytes = 0; // Sum of live[].
		uint64_t unattributed_bytes = 0; // device_bytes the categories do not explain.
		uint64_t pending_bytes = 0; // Sum of pending[].
		uint64_t headroom_bytes = 0;
		uint64_t live[CATEGORY_COUNT] = {};
		uint64_t pending[CATEGORY_COUNT] = {};
		uint64_t refused_reservations = 0;
		uint64_t report_only_overruns = 0;
	};

	// Releases its reservation when it goes out of scope, so that no failure path can leak one.
	class ScopedReservation {
		DeviceVramLedger &ledger;
		Category category;
		bool held = false;

	public:
		ScopedReservation(DeviceVramLedger &p_ledger, Category p_category) :
				ledger(p_ledger), category(p_category) {}
		ScopedReservation(const ScopedReservation &) = delete;
		ScopedReservation &operator=(const ScopedReservation &) = delete;
		~ScopedReservation() {
			if (held) { ledger.release(category); }
		}
		bool acquire(uint64_t p_bytes) {
			const bool ok = ledger.reserve(category, p_bytes);
			held = held || ok;
			return ok;
		}
	};

	// `p_limit_clamp` != 0 lowers the limit (the test hook of the HLOD frame route).
	void configure(uint64_t p_device_heap_bytes, uint64_t p_fixed_cap_bytes, uint64_t p_limit_clamp = 0) {
		uint64_t limit = gs_limit_for_device(p_device_heap_bytes, p_fixed_cap_bytes);
		if (p_limit_clamp != 0 && p_limit_clamp < limit) { limit = p_limit_clamp; }
		device_heap_bytes.store(p_device_heap_bytes, std::memory_order_relaxed);
		limit_bytes.store(limit, std::memory_order_relaxed);
		configured.store(true, std::memory_order_relaxed);
	}
	uint64_t limit() const { return limit_bytes.load(std::memory_order_relaxed); }
	// The atlas limit of the configured heap. The override setting is read by the caller; this class reads no settings.
	uint64_t atlas_limit(uint32_t p_override_mb) const {
		return atlas_limit_for_device(device_heap_bytes.load(std::memory_order_relaxed), p_override_mb);
	}

	void observe_device_bytes(uint64_t p_bytes) {
		device_bytes.store(p_bytes, std::memory_order_relaxed);
		device_observed.store(true, std::memory_order_relaxed);
	}
	// A measurement an HLOD frame was admitted with; published as hlod_owned_byte_upper_bound.
	void admit_device_bytes(uint64_t p_bytes) {
		observe_device_bytes(p_bytes);
		admitted_device_bytes.store(p_bytes, std::memory_order_relaxed);
	}

	// The bytes the category holds now. Replaces the previous report: the same memory reported again is counted once.
	void report(Category p_category, uint64_t p_bytes) {
		live[p_category].store(p_bytes, std::memory_order_relaxed);
		if (!is_enforced(p_category) && configured.load(std::memory_order_relaxed) && _used() > limit()) {
			report_only_overruns.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// Pre-flight `p_bytes` not yet allocated. Replaces the category's own reservation, so a retry never stacks. True when
	// it fits next to the measurement and the other categories' reservations. A refused reservation changes nothing but
	// the refusal counter. An unconfigured ledger has limit 0 and refuses (fail closed). A report-only category always
	// succeeds and counts the overrun.
	bool reserve(Category p_category, uint64_t p_bytes) {
		const uint64_t used = _saturating_add(_basis(), _pending_total(p_category));
		const uint64_t max_bytes = limit();
		const bool fits = used <= max_bytes && p_bytes <= max_bytes - used;
		if (!fits && is_enforced(p_category)) {
			refused_reservations.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (!fits && configured.load(std::memory_order_relaxed)) {
			report_only_overruns.fetch_add(1, std::memory_order_relaxed);
		}
		pending[p_category].store(p_bytes, std::memory_order_relaxed);
		return true;
	}
	void release(Category p_category) { pending[p_category].store(0, std::memory_order_relaxed); }

	// What can still be reserved. 0 when the limit is already exceeded.
	uint64_t headroom() const {
		const uint64_t used = _used();
		const uint64_t max_bytes = limit();
		return used < max_bytes ? max_bytes - used : 0;
	}

	Snapshot snapshot() const {
		Snapshot out;
		out.device_heap_bytes = device_heap_bytes.load(std::memory_order_relaxed);
		out.limit_bytes = limit();
		out.engine_driver_reserve_bytes = out.device_heap_bytes > out.limit_bytes ? out.device_heap_bytes - out.limit_bytes : 0;
		out.device_observed = device_observed.load(std::memory_order_relaxed);
		out.device_bytes = device_bytes.load(std::memory_order_relaxed);
		out.admitted_device_bytes = admitted_device_bytes.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < CATEGORY_COUNT; ++i) {
			out.live[i] = live[i].load(std::memory_order_relaxed);
			out.pending[i] = pending[i].load(std::memory_order_relaxed);
			out.attributed_bytes = _saturating_add(out.attributed_bytes, out.live[i]);
			out.pending_bytes = _saturating_add(out.pending_bytes, out.pending[i]);
		}
		out.unattributed_bytes = out.device_bytes > out.attributed_bytes ? out.device_bytes - out.attributed_bytes : 0;
		out.headroom_bytes = headroom();
		out.refused_reservations = refused_reservations.load(std::memory_order_relaxed);
		out.report_only_overruns = report_only_overruns.load(std::memory_order_relaxed);
		return out;
	}

private:
	static uint64_t _saturating_add(uint64_t p_a, uint64_t p_b) { return p_b > UINT64_MAX - p_a ? UINT64_MAX : p_a + p_b; }

	uint64_t _basis() const {
		if (device_observed.load(std::memory_order_relaxed)) { return device_bytes.load(std::memory_order_relaxed); }
		uint64_t sum = 0;
		for (uint32_t i = 0; i < CATEGORY_COUNT; ++i) {
			sum = _saturating_add(sum, live[i].load(std::memory_order_relaxed));
		}
		return sum;
	}
	uint64_t _pending_total(int p_except = -1) const {
		uint64_t sum = 0;
		for (int i = 0; i < CATEGORY_COUNT; ++i) {
			if (i != p_except) { sum = _saturating_add(sum, pending[i].load(std::memory_order_relaxed)); }
		}
		return sum;
	}
	uint64_t _used() const { return _saturating_add(_basis(), _pending_total()); }

	std::atomic<uint64_t> device_heap_bytes{ 0 };
	std::atomic<uint64_t> limit_bytes{ 0 };
	std::atomic<bool> configured{ false };
	std::atomic<bool> device_observed{ false };
	std::atomic<uint64_t> device_bytes{ 0 };
	std::atomic<uint64_t> admitted_device_bytes{ 0 };
	std::atomic<uint64_t> live[CATEGORY_COUNT] = {};
	std::atomic<uint64_t> pending[CATEGORY_COUNT] = {};
	std::atomic<uint64_t> refused_reservations{ 0 };
	std::atomic<uint64_t> report_only_overruns{ 0 };
};

#endif // GS_DEVICE_VRAM_LEDGER_H
