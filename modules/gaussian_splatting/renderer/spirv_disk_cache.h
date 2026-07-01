#ifndef GAUSSIAN_SPLATTING_SPIRV_DISK_CACHE_H
#define GAUSSIAN_SPLATTING_SPIRV_DISK_CACHE_H

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <cstdint>

class RenderingDevice;

class SPIRVDiskCache {
public:
    static SPIRVDiskCache *get();
    static void shutdown();

    bool is_enabled() const;
    void set_enabled(bool p_enabled);

    String compute_key(const String &p_source, const Vector<String> &p_defines, RenderingDevice *p_device);

    // try_load/store/invalidate take the device so the per-device subdir is
    // resolved under the same mutex critical section as the file op. Without
    // this, two threads compiling against different RDs could flip the cached
    // device dir between compute_key() and try_load()/store(), causing reads
    // and writes to route to the wrong device subdirectory.
    bool try_load(const String &p_key, RenderingDevice *p_device, Vector<uint8_t> &r_spirv);
    void store(const String &p_key, RenderingDevice *p_device, const Vector<uint8_t> &p_spirv);
    void invalidate(const String &p_key, RenderingDevice *p_device);

    void prune_above(uint64_t p_max_bytes);

private:
    SPIRVDiskCache() = default;

    String _device_fingerprint(RenderingDevice *p_device);
    String _device_short_hash(RenderingDevice *p_device);
    String _cache_dir_for_device(RenderingDevice *p_device);
    bool _ensure_dir(const String &p_dir);

    mutable Mutex mutex;
    String cached_device_dir;
    uint64_t cached_device_id = 0;
    bool enabled = true;
};

#endif // GAUSSIAN_SPLATTING_SPIRV_DISK_CACHE_H
