#ifndef GS_GPU_RECORD_SIZES_H
#define GS_GPU_RECORD_SIZES_H

#include <cstdint>

// RenderingDevice buffer sizes are uint32_t. Validate before narrowing, including
// transfer alignment; no logical record capacity may wrap into a small buffer.
inline bool gs_gpu_record_buffer_size(uint32_t p_count, uint32_t p_stride,
        uint32_t p_alignment, uint32_t &r_bytes) {
    r_bytes = 0;
    if (p_stride == 0 || p_alignment == 0 || (p_alignment & (p_alignment - 1)) != 0) {
        return false;
    }
    const uint64_t raw_bytes = uint64_t(p_count) * p_stride;
    const uint64_t aligned_bytes = (raw_bytes + p_alignment - 1) & ~(uint64_t(p_alignment) - 1);
    if (aligned_bytes > UINT32_MAX) {
        return false;
    }
    r_bytes = uint32_t(aligned_bytes);
    return true;
}

// Scratch records must cover *every byte* of an aligned upload. In particular,
// 176-byte records do not divide 256-byte transfers. Round up, never down.
inline uint32_t gs_gpu_upload_scratch_count(uint32_t p_bytes, uint32_t p_stride) {
    return p_stride > 0 ? uint32_t((uint64_t(p_bytes) + p_stride - 1) / p_stride) : 0;
}

#endif // GS_GPU_RECORD_SIZES_H
