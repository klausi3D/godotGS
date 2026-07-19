#ifndef GS_SYNC_POLICY_H
#define GS_SYNC_POLICY_H

#include "core/object/ref_counted.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

// Helper functions to safely submit/sync RenderingDevice
// Only the main/local RenderingDevice can call submit() and sync()
namespace gs_device_utils {

inline bool is_local_device(RenderingDevice *p_device) {
    if (!p_device) {
        return false;
    }
    // Local devices are NOT the main instance and CAN call submit/sync
    return !p_device->is_main_rendering_device();
}

// True when p_device is a local device that has been submit()ed and not yet
// sync()ed. A local device in that state has already ended its command buffer and
// its draw graph, so anything that re-enters the frame lifecycle -- a second
// submit(), or a synchronous readback, which stalls through
// _flush_and_stall_for_all_frames() -> _end_frame() -- runs against ended state
// and faults inside the driver. (GS #685)
inline bool has_outstanding_submit(RenderingDevice *p_device) {
    return p_device && is_local_device(p_device) && p_device->is_local_device_submission_pending();
}

// Complete an outstanding submit() so the device is back in a recording state.
// Returns true when a sync() was actually issued.
//
// This is the enforceable half of the local-device submission contract: rather
// than documenting "do not read back while a submit is in flight" and relying on
// call ordering to honour it, every helper below settles first, so the illegal
// state cannot be observed by the operation that would fault on it. Callers pay
// nothing they were not already paying -- a synchronous readback stalls for all
// frames regardless, and a second submit() would otherwise have been rejected
// outright by RenderingDevice::submit()'s "device already submitted" guard.
inline bool settle_outstanding_submit(RenderingDevice *p_device) {
    if (!has_outstanding_submit(p_device)) {
        return false;
    }
    p_device->sync();
    return true;
}

// Safe submit - only submits on local (non-main) devices.
//
// On a local device the submission is COMPLETED here, not left in flight. That is
// not caution, it is the only representable behaviour: RenderingDevice::submit()
// runs _end_frame(), so the frame's command buffer and draw graph are ended, and
// only sync() -> _begin_frame() reopens them. Every caller in this module keeps
// going afterwards -- more compute lists, more buffer_updates, more readbacks --
// and all of that would be recorded into an ended command buffer and an ended
// draw graph, which the next _end_frame() then replays. That is a driver fault,
// observed as VK_ERROR_DEVICE_LOST on the instance cull -> sort path, and as a
// direct crash in _capture_instance_count_sync (#685).
//
// Nothing in this module ever paired safe_submit with a later sync, so the
// in-flight state had no owner and no consumer -- it existed only as a window in
// which every following device call was illegal.
//
// This is a no-op on the main RenderingDevice, which is what every shipping
// configuration uses (GaussianSplatManager::get_primary_rendering_device()), so
// production behaviour is unchanged; it defines the local-device path the GPU
// test harness runs on.
inline void safe_submit(RenderingDevice *p_device) {
    if (p_device && is_local_device(p_device)) {
        settle_outstanding_submit(p_device);
        p_device->submit();
        p_device->sync();
    }
    // Main device: submit is handled automatically by Godot's frame
}

// Safe sync - completes an outstanding local-device submission, if there is one.
//
// Deliberately NOT an unconditional sync(): RenderingDevice::sync() ERR_FAILs with
// "sync can only be called after a submit" when nothing is in flight, and since
// safe_submit above completes its own submission, that is now the normal state.
// Call sites that pair safe_submit with safe_sync therefore still read correctly
// and the second call is simply a no-op. (GS #685)
inline void safe_sync(RenderingDevice *p_device) {
    settle_outstanding_submit(p_device);
    // Main device: sync is handled automatically by Godot's frame
}

// Safe submit and sync combo
inline void safe_submit_and_sync(RenderingDevice *p_device) {
    if (p_device && is_local_device(p_device)) {
        settle_outstanding_submit(p_device);
        p_device->submit();
        p_device->sync();
    }
    // Main device: handled automatically by Godot's frame
}

// Synchronous buffer readback that is legal regardless of submission state.
//
// RenderingDevice::buffer_get_data() stalls through
// _flush_and_stall_for_all_frames(), which calls _end_frame() unconditionally. On
// a local device with an outstanding submit() that re-ends an already-ended
// command buffer and draw graph, and the driver faults replaying the graph. Every
// synchronous readback in this module goes through here so the invariant is
// enforced in one place rather than restated at each call site. On the main
// device this is exactly buffer_get_data(). (GS #685)
inline Vector<uint8_t> safe_buffer_get_data(RenderingDevice *p_device, RID p_buffer,
        uint32_t p_offset = 0, uint32_t p_size = 0) {
    if (!p_device) {
        return Vector<uint8_t>();
    }
    settle_outstanding_submit(p_device);
    return p_device->buffer_get_data(p_buffer, p_offset, p_size);
}

// Same contract as safe_buffer_get_data for the texture readback path:
// RenderingDevice::texture_get_data() stalls through the identical
// _flush_and_stall_for_all_frames() call. (GS #685)
inline Vector<uint8_t> safe_texture_get_data(RenderingDevice *p_device, RID p_texture, uint32_t p_layer) {
    if (!p_device) {
        return Vector<uint8_t>();
    }
    settle_outstanding_submit(p_device);
    return p_device->texture_get_data(p_texture, p_layer);
}

} // namespace gs_device_utils

namespace gs_sort_policy {

enum class ReadbackMode {
    STRICT_ASYNC,
    ASYNC_WITH_SYNC_BOOTSTRAP,
    STRICT_SYNC,
    DEBUG_VALIDATION,
};

struct ReadbackPolicy {
    ReadbackMode mode = ReadbackMode::STRICT_ASYNC;
    bool allow_sync_readback = false;
    bool allow_sync_sort_fallback = false;
    bool allow_sync_bootstrap = false;
    bool allow_sync_pending_readback = false;
    bool allow_sync_enqueue_fallback = false;
};

inline const char *mode_name(ReadbackMode p_mode) {
    switch (p_mode) {
        case ReadbackMode::STRICT_ASYNC:
            return "strict_async";
        case ReadbackMode::ASYNC_WITH_SYNC_BOOTSTRAP:
            return "async_with_sync_bootstrap";
        case ReadbackMode::STRICT_SYNC:
            return "strict_sync";
        case ReadbackMode::DEBUG_VALIDATION:
            return "debug_validation";
        default:
            return "strict_async";
    }
}

inline ReadbackPolicy make_policy(ReadbackMode p_mode) {
    ReadbackPolicy policy;
    policy.mode = p_mode;
    switch (p_mode) {
        case ReadbackMode::STRICT_ASYNC: {
            break;
        }
        case ReadbackMode::ASYNC_WITH_SYNC_BOOTSTRAP: {
            policy.allow_sync_bootstrap = true;
            // Keep startup correctness with a single bootstrap sample, but
            // preserve strict async behavior in steady-state to avoid recurring
            // CPU/GPU stalls from pending/enqueue sync readbacks.
            policy.allow_sync_pending_readback = false;
            policy.allow_sync_enqueue_fallback = false;
            break;
        }
        case ReadbackMode::STRICT_SYNC:
        case ReadbackMode::DEBUG_VALIDATION: {
            policy.allow_sync_readback = true;
            policy.allow_sync_sort_fallback = true;
            policy.allow_sync_bootstrap = true;
            policy.allow_sync_pending_readback = true;
            policy.allow_sync_enqueue_fallback = true;
            break;
        }
        default:
            break;
    }
    return policy;
}

inline ReadbackPolicy resolve_readback_policy(bool p_debug_sync_requested, bool p_preserve_gpu_timestamps) {
    if (p_debug_sync_requested && !p_preserve_gpu_timestamps) {
        return make_policy(ReadbackMode::DEBUG_VALIDATION);
    }
    if (p_preserve_gpu_timestamps) {
        return make_policy(ReadbackMode::STRICT_ASYNC);
    }
    return make_policy(ReadbackMode::ASYNC_WITH_SYNC_BOOTSTRAP);
}

} // namespace gs_sort_policy

// Pure abstract interface - implementations should inherit from RefCounted
class ISyncPolicy {
public:
    virtual ~ISyncPolicy() = default;
    virtual bool sync(RenderingDevice *p_device, const char *p_context = nullptr) = 0;
};

class CoarseSyncPolicy : public RefCounted, public ISyncPolicy {
    GDCLASS(CoarseSyncPolicy, RefCounted);

protected:
    static void _bind_methods() {}

public:
    bool sync(RenderingDevice *p_device, const char *p_context = nullptr) override {
        if (p_device == nullptr) {
            return false;
        }

        // Use safe variants to avoid errors on main device
        gs_device_utils::safe_submit_and_sync(p_device);
        return true;
    }
};

#endif // GS_SYNC_POLICY_H
