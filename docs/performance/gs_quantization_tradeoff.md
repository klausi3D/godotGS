# Per-chunk quantization: the VRAM ↔ compute tradeoff

**Setting:** `rendering/gaussian_splatting/compression/per_chunk_quantization`
(`-1` = auto from quality tier, `0` = off, `1` = on). **Default: off.**

Per-chunk quantization stores each splat in the **80-byte `PackedGaussianQuantized`**
layout instead of the 144-byte `PackedGaussian` — a **−44 % atlas VRAM per splat**.
Position and scale are quantized per chunk (16-bit into per-chunk min/range bounds),
rotation and normal are FP16, opacity and SH-DC stay FP32.

**This is not a free win.** The saving is bandwidth/VRAM; the cost is compute. The GPU
must **dequantize every splat every frame** — unpack position/scale/rotation from the 80-byte
struct and do two extra buffer reads for the per-chunk bounds — in the binning, depth, and
raster passes. Whether that is a net win depends entirely on whether you are VRAM-bound.

## Measured (RTX 3090, optimized build)

**Render quality** — resident path, real-scan asset (Rose), both configs rendering the
**identical** splat set so this isolates quantization error:

| metric | value |
| --- | --- |
| PSNR (quant vs full) | **40.6 dB** (near-lossless for a −44 % layout) |
| max per-channel diff | 119 / 255 |
| mean diff | 0.31 / 255 |

**Frame time** — dense-2M resident lane, quant off vs on at an **identical 1.96 M splat
count** (no LOD-thinning confound), so this isolates the dequantization ALU cost:

| | p50 | p99 | avg | fps |
| --- | --- | --- | --- | --- |
| full (144 B) | 33.3 ms | 34.7 ms | 33.9 | 29.5 |
| quant (80 B) | 37.9 ms | 40.4 ms | 38.4 | 26.0 |
| **delta** | **+13.6 %** | **+16.2 %** | +13.2 % | −11.9 % |

On a high-end GPU with fast VRAM there is no bandwidth pressure at 2 M splats, so the
dequant ALU shows up as a straight ~14 % frame-time regression.

**VRAM headroom** — at a *fixed* VRAM budget the 80-byte atlas fits far more splats before
the resident importance-clamp thins the scene: measured **1.2 M vs 235 k** splats on
baeume-lankow (~5× more content, or the same content at ~55 % of the atlas bytes).

## When to enable

**Enable** when you are **VRAM-bound**:
- A scene that would otherwise exceed the resident VRAM budget and get importance-thinned
  (or fail to fit at all) — quantization is the difference between rendering the full scene
  and dropping splats.
- Low-end / mobile GPUs where VRAM is the wall (see `gs_vram_reduction_plan.md`).
- Fitting ~2× more splats in the same budget when splat count, not frame time, is the limit.

**Leave off** when you have VRAM headroom: the same splats render ~14 % slower for no benefit.

**Do not blanket-enable it** (e.g. as a tier default) without measuring frame time on the
target hardware — the trade flips sign with available VRAM.

## Caveats

- **Position/scale bit depth is capped at 16** (the `quantized_position`/`quantized_scale`
  fields are `uint16`). The `position_bits` setting is clamped to 16 for the quantized atlas
  regardless of its 8–24 range (`GS_QUANTIZED_BITS_MAX`).
- **Quantized binning zeroes normals** (`tile_binning.glsl`), so **quantized content is
  effectively unlit**. Fine for the current unlit real-scan target; a blocker for lit content
  until the shader carries normals through the quantized path.
- Scale quantization is **mandatory** for the 80-byte layout (it has no unquantized scale
  field); it is forced on internally when quantization is enabled.

## References

- Resident implementation: `renderer/resident_instance_contract_publisher.cpp` (GS-PERF-Q80B).
- Packer: `renderer/gaussian_gpu_layout.cpp` `pack_gaussian_quantized()` (GS-PERF-Q80A).
- GLSL dequant: `shaders/includes/quantization_dequant.glsl`.
- VRAM strategy: `gs_vram_reduction_plan.md`.
