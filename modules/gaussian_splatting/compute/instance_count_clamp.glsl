#[compute]

#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

#include "../shaders/includes/platform_compat.glsl"

#ifndef GS_DISPATCH_LOCAL_SIZE_X
#define GS_DISPATCH_LOCAL_SIZE_X 256
#endif

layout(set = 0, binding = 0, std430) buffer CounterBuffer {
    uint visible_splat_count;
    uint overflowed_splats;
} counters;

layout(set = 0, binding = 1, std430) buffer IndirectDispatch {
    uint dispatch_xyz[3];
    uint element_count;
    uint overflow_flag;
    uint unclamped_total;
} indirect_dispatch;

layout(set = 0, binding = 3, std430) buffer InstanceCount {
    uint dispatch_xyz[3];
    uint element_count;
    uint overflow_flag;
    uint unclamped_total;
} instance_count;

layout(set = 0, binding = 2, std140) uniform Params {
    mat4 view_matrix;
    uint visible_chunk_count;
    uint max_visible_splats;
    uint pad0;
    // C4b (G4), Channel B: 1 => the CPU consumed the sticky overflow_flag on the previous
    // run, so reset the accumulation this frame; 0 => accumulate (atomicMax). See below.
    uint consume_overflow_flag;
} params;

// Clamp indirect instance counts to the configured dispatch budget.
void main() {
    uint raw_count = counters.visible_splat_count;
    uint max_visible = params.max_visible_splats;
    uint clamped = min(raw_count, max_visible);

    uint dispatch_x = (clamped + GS_DISPATCH_LOCAL_SIZE_X - 1u) / GS_DISPATCH_LOCAL_SIZE_X;
    uint overflow_now = (raw_count > max_visible || counters.overflowed_splats > 0u) ? 1u : 0u;

    indirect_dispatch.dispatch_xyz[0] = dispatch_x;
    indirect_dispatch.dispatch_xyz[1] = 1u;
    indirect_dispatch.dispatch_xyz[2] = 1u;
    indirect_dispatch.element_count = clamped;
    indirect_dispatch.unclamped_total = raw_count;
    // Per-frame flag: the indirect_count buffer is not the sampled telemetry channel
    // (nothing reads its overflow_flag back), so it keeps the current-frame value.
    indirect_dispatch.overflow_flag = overflow_now;

    instance_count.dispatch_xyz[0] = dispatch_x;
    instance_count.dispatch_xyz[1] = 1u;
    instance_count.dispatch_xyz[2] = 1u;
    instance_count.element_count = clamped;
    instance_count.unclamped_total = raw_count;
    // C4b (G4), Channel B: the async instance-count readback that samples this buffer is
    // gated on !pending, so on a frame whose readback is skipped an overwritten per-frame
    // flag would be lost -- a later sample reads 0 and the clamp is silent. Make the flag
    // STICKY: accumulate with atomicMax so a clamp on ANY frame persists until a reader
    // consumes it. The CPU sets consume_overflow_flag=1 on the frame after it sampled the
    // buffer (async enqueue or sync capture), which resets the accumulation so each readback
    // interval reports at most once. This mirrors the drop-signal re-arm (Channel A), and the
    // reset here (in the writer, before the accumulate) is GPU-ordered strictly after the
    // prior frame's readback copy -- so it clears only the value already sampled.
    // Single invocation (local_size 1,1,1); atomicMax is used to match the drop-signal idiom.
    if (params.consume_overflow_flag != 0u) {
        instance_count.overflow_flag = overflow_now;
    } else {
        atomicMax(instance_count.overflow_flag, overflow_now);
    }
}
