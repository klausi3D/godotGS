# ADR: full SH0–4 GPU record storage

- Status: accepted for implementation under the approved v1 roadmap; merge requires R3 review and maintainer disposition.
- Date: 2026-10-08
- Risk: R3 (host/shader ABI and cache compatibility)
- Base: `0d6b828ed7544f6e222faed6e798462ca0163650`, stacked on SPZ adapter PR #1201.

## Problem and boundary

Import and GSF v3 now retain all 24 non-DC RGB coefficients of SH4. The normal GPU record stores twelve, and the position/scale-quantized record stores six. Both silently lose SH3 terms before evaluation. Close that storage gap in one independently reviewable change before extending evaluation and degree-selection policies.

This task changes GPU record storage, upload decoding and ABI verification only. It does not expose degree 4 as an evaluated quality mode, implement antialias compensation, change LOD quality policy, or claim complete SPZ v4 rendering. The existing degree-3 evaluator remains bounded to its 16 basis values. Correct SH mathematics and full degree-4 evaluation require their own task and numerical/GPU evidence.

## Decision

Both record types store 24 signed SNORM10 RGB words with the existing per-splat scale and coefficient order: up to three first-order coefficients, then up to 21 higher-order coefficients. Keep metadata masks and encoding ID 2; eight-bit counts already cover this capacity. Coefficient limits remain explicit and bounded by the record capacity. Zero unused words and all padding deterministically.

The normal record grows from 128 to 176 bytes: DC at 48, encoded words at 64, normal at 160, metadata at 172. The quantized record grows from 80 to 160 bytes: DC at 32, encoded words at 48, normal at 144/148 and two explicit zero padding words at 152/156. Mirror scalar arrays and trailing padding in std430 exactly. Quantized shader decoding copies all 24 words and synthesizes capacity metadata; zero unused coefficients retain the existing semantics.

All allocation/upload/copy/budget paths must use actual `sizeof` strides. Existing source/config fingerprints invalidate packed payloads; bump the SPIR-V disk cache version as additional protection against old ABI shaders. No disk GSF or import schema changes are needed. Do not hand-edit generated shader headers.

## Tradeoff and validation

This intentionally increases fixed GPU storage (48 bytes for normal, 80 bytes for quantized records) to preserve supported content; no speed or memory improvement is claimed. Adaptive storage is research work after measured quality equivalence.

Extend host/shader layout verification without removing field, offset, size or signed-storage checks. Add meaningful native roundtrips for every SH0–4 term in both packers, coefficient-limit tails, deterministic padding, and per-splat scale coverage. Existing strict SHEncoding and renderer-layout lanes execute them. Mutation-prove capacity/tail preservation; compile the production shader matrix. Runtime GPU, real-scan images, resource/budget behavior and performance evidence remain required before merge; headless passes cannot substitute.

## Rollback

Revert this ABI change as one unit, including shader mirrors and cache version. Keep imported/persisted SH4 intact. The v1 candidate must not be released with the old capacity or without the subsequent SH4 evaluation and antialias tasks.

## Allocation and aligned-upload qualification

The 176-byte stride no longer divides the 256-byte transfer alignment. Scratch storage must use ceiling division and zero the entire submitted padding range. Compute record, combined and aligned byte sizes with checked 64-bit arithmetic before narrowing to RenderingDevice's 32-bit buffer API. Reject unsupported capacities before allocation; do not wrap or silently clamp the logical record capacity. Native boundary checks must exercise these calculations without allocating large buffers, and actual small-upload readback remains GPU evidence.

