#ifndef GS_REGION_OVERRIDE_BUFFER_H
#define GS_REGION_OVERRIDE_BUFFER_H

#include "gaussian_gpu_layout.h"
#include "tile_render_types.h"
#include <cstring>

// Single owner, render-thread only. Uniform sets borrow the RID and are evicted
// by RD when it is freed. clear() is required on device teardown and failures.
class GSRegionOverrideBuffer {
    RID buffer;
    GaussianSplatting::BufferOwnership owner;
    LocalVector<RegionOverrideGPU> uploaded;
    uint32_t capacity = 0;
    uint64_t generation = 0;
#ifdef TESTS_ENABLED
    bool defer_upload = false;
    LocalVector<RegionOverrideGPU> deferred;
#endif
public:
    ~GSRegionOverrideBuffer() { clear(); }
    GSRegionOverrideBuffer() = default;
    GSRegionOverrideBuffer(const GSRegionOverrideBuffer &) = delete;
    GSRegionOverrideBuffer &operator=(const GSRegionOverrideBuffer &) = delete;
    RID get() const { return buffer; }
    uint64_t revision() const { return generation; }
    uint64_t bytes() const { return uint64_t(capacity) * sizeof(RegionOverrideGPU); }
    static RegionOverrideGPU neutral() {
        RegionOverrideGPU value = {};
        value.tint[0] = value.tint[1] = value.tint[2] = 1;
        value.emissive_opacity[1] = 0x3c000000u; // fp16 opacity 1, emissive B 0.
        return value;
    }
    void clear() {
        if (buffer.is_valid() && owner.device) { owner.device->free(buffer); }
        buffer = RID(); owner.clear(); uploaded.clear(); capacity = 0;
    }
    bool ensure_neutral(RenderingDevice *p_rd) {
        LocalVector<RegionOverrideGPU> values; values.push_back(neutral());
        return update(p_rd, values);
    }
#ifdef TESTS_ENABLED
    void test_defer_one_frame(bool p_enabled) { defer_upload = p_enabled; deferred.clear(); }
    bool test_is_deferred() const { return defer_upload; }
    void test_set_revision(uint64_t p_revision) { generation = p_revision; }
#endif
    bool update(RenderingDevice *p_rd, const LocalVector<RegionOverrideGPU> &p_entries) {
#ifdef TESTS_ENABLED
        if (defer_upload) {
            LocalVector<RegionOverrideGPU> previous = deferred.is_empty() ? uploaded : deferred;
            if (previous.is_empty()) { previous.push_back(neutral()); }
            deferred = p_entries;
            return upload(p_rd, previous);
        }
#endif
        return upload(p_rd, p_entries);
    }
private:
    bool upload(RenderingDevice *p_rd, const LocalVector<RegionOverrideGPU> &p_entries) {
        if (!p_rd || p_entries.is_empty() || uint64_t(p_entries.size()) * sizeof(RegionOverrideGPU) > UINT32_MAX) { clear(); return false; }
        if (buffer.is_valid() && !owner.matches(p_rd)) { clear(); }
        if (!buffer.is_valid() || capacity < p_entries.size()) {
            clear();
            const uint64_t size = uint64_t(p_entries.size()) * sizeof(RegionOverrideGPU);
            Vector<uint8_t> data;
            if (data.resize(size) != OK) { return false; }
            memcpy(data.ptrw(), p_entries.ptr(), size);
            buffer = p_rd->storage_buffer_create(uint32_t(size), data);
            if (!buffer.is_valid()) { return false; }
            owner.set(p_rd); capacity = p_entries.size();
            p_rd->set_resource_name(buffer, "GS_RegionOverrides");
            uploaded = p_entries; ++generation;
            return true;
        }
        bool changed = uploaded.size() != p_entries.size();
        for (uint32_t first = 0; first < p_entries.size();) {
            if (first < uploaded.size() && memcmp(&uploaded[first], &p_entries[first], sizeof(RegionOverrideGPU)) == 0) { ++first; continue; }
            uint32_t end = first + 1;
            while (end < p_entries.size() && (end >= uploaded.size() || memcmp(&uploaded[end], &p_entries[end], sizeof(RegionOverrideGPU)) != 0)) { ++end; }
            if (p_rd->buffer_update(buffer, first * sizeof(RegionOverrideGPU), (end - first) * sizeof(RegionOverrideGPU), p_entries.ptr() + first) != OK) { clear(); return false; }
            changed = true; first = end;
        }
        uploaded = p_entries;
        if (changed) { ++generation; }
        return true;
    }
};

#endif
