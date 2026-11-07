#ifndef STREAMING_LATENCY_HISTOGRAM_H
#define STREAMING_LATENCY_HISTOGRAM_H

#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
#include <cstdint>

// BV-04a (ADR adr-beyond-vram-worlds.md D-5): a fixed log2 latency histogram whose counters
// are lock-free atomics in EVERY build flavour, so release-build streaming latencies are
// measurable. One record() is one atomic add per field (bucket, samples, total) plus one
// compare-exchange for the max; there is no per-splat work and no allocation.
//
// Bucket b holds samples in [2^(b-1), 2^b) microseconds for 1 <= b < BUCKET_COUNT - 1,
// bucket 0 holds samples of 0 us, and the last bucket is open-ended (>= 2^(BUCKET_COUNT-2) us,
// about 4.2 s). Counts are cumulative over the owner's lifetime and never reset per frame, so
// a reader that samples only some frames loses nothing: it diffs two snapshots.
struct StreamingLatencyHistogram {
    static constexpr uint32_t BUCKET_COUNT = 24;

    SafeNumeric<uint64_t> buckets[BUCKET_COUNT];
    SafeNumeric<uint64_t> samples;
    SafeNumeric<uint64_t> total_usec;
    SafeNumeric<uint64_t> max_usec;

    static constexpr uint32_t bucket_for(uint64_t p_usec) {
        uint32_t width = 0;
        while (p_usec != 0 && width < BUCKET_COUNT - 1) {
            p_usec >>= 1;
            width++;
        }
        return width;
    }

    // Exclusive upper bound of bucket p_bucket in microseconds; -1 for the open-ended last one.
    static constexpr int64_t bucket_upper_usec(uint32_t p_bucket) {
        return p_bucket >= BUCKET_COUNT - 1 ? int64_t(-1) : (int64_t(1) << p_bucket);
    }

    _ALWAYS_INLINE_ void record(uint64_t p_usec) {
        buckets[bucket_for(p_usec)].increment();
        samples.increment();
        total_usec.add(p_usec);
        max_usec.exchange_if_greater(p_usec);
    }

    struct Snapshot {
        uint64_t buckets[BUCKET_COUNT] = {};
        uint64_t samples = 0;
        uint64_t total_usec = 0;
        uint64_t max_usec = 0;
    };

    Snapshot read() const {
        Snapshot s;
        for (uint32_t i = 0; i < BUCKET_COUNT; i++) {
            s.buckets[i] = buckets[i].get();
        }
        s.samples = samples.get();
        s.total_usec = total_usec.get();
        s.max_usec = max_usec.get();
        return s;
    }

    static Dictionary snapshot_to_dictionary(const Snapshot &p_snapshot) {
        Array counts;
        counts.resize(BUCKET_COUNT);
        for (uint32_t i = 0; i < BUCKET_COUNT; i++) {
            counts[i] = int64_t(p_snapshot.buckets[i]);
        }
        Dictionary d;
        d["bucket_counts"] = counts;
        d["samples"] = int64_t(p_snapshot.samples);
        d["total_usec"] = int64_t(p_snapshot.total_usec);
        d["max_usec"] = int64_t(p_snapshot.max_usec);
        return d;
    }

    static Array bucket_upper_bounds_usec() {
        Array bounds;
        bounds.resize(BUCKET_COUNT);
        for (uint32_t i = 0; i < BUCKET_COUNT; i++) {
            bounds[i] = bucket_upper_usec(i);
        }
        return bounds;
    }
};

#endif // STREAMING_LATENCY_HISTOGRAM_H
