# Per-chunk quantization: the VRAM ↔ compute tradeoff

**Setting:** `rendering/gaussian_splatting/compression/per_chunk_quantization`
(`-1` = auto from quality tier, `0` = off, `1` = on). **Default: `-1` (auto).** Auto resolves to
off under the default `rendering/gaussian_splatting/quality/tier_preset` (`custom`) and under every
tier except `steam_deck`, which turns quantization **on** (`QuantizationConfig::load_from_project_settings()`,
`get_quality_tier_config()`).

The current full-SH storage contract uses **160-byte `PackedGaussianQuantized`**
records and **176-byte `PackedGaussian`** records. Both retain all 24 non-DC SH4
coefficients. The record-byte reduction is about **9.1%**; this is arithmetic,
not a measured reduction in complete scene VRAM. Position and scale remain
quantized per chunk, rotation and normal are FP16, opacity and SH-DC stay FP32.
See [Full SH GPU Storage](../architecture/adr-full-sh-gpu-storage.md).

**The measurements below are historical.** They compare the older 144-byte
normal layout with an 80-byte quantized layout; normal records later shrank to
128 bytes before this SH extension. These figures have not been rerun for the
176/160-byte layouts. They do not establish current frame time, image quality,
scene capacity, or the benefit of a quality-tier default.

Quantization adds GPU dequantization and per-chunk bounds reads. Measure its
cost against its smaller record size on the target scene and hardware.

## Historical measurements (RTX 3090, optimized build; 144/80-byte records)

**Render quality** — resident path, real-scan asset (Rose), both configs rendering the
**identical** splat set so this isolates quantization error:

| metric | value |
| --- | --- |
| PSNR (quant vs full) | **40.6 dB** (near-lossless; measured against the then-144 B layout) |
| max per-channel diff | 119 / 255 |
| mean diff | 0.31 / 255 |

**Frame time** — dense-2M resident lane, quant off vs on at an **identical 1.96 M splat
count** (no LOD-thinning confound), so this isolates the dequantization ALU cost:

| | p50 | p99 | avg | fps |
| --- | --- | --- | --- | --- |
| full (144 B, pre-#971) | 33.3 ms | 34.7 ms | 33.9 | 29.5 |
| quant (80 B) | 37.9 ms | 40.4 ms | 38.4 | 26.0 |
| **delta** | **+13.6 %** | **+16.2 %** | +13.2 % | −11.9 % |

On a high-end GPU with fast VRAM there is no bandwidth pressure at 2 M splats, so the
dequant ALU shows up as a straight ~14 % frame-time regression.

**VRAM headroom** — at a *fixed* VRAM budget the 80-byte atlas fits far more splats before
the resident importance-clamp thins the scene: measured **1.2 M vs 235 k** splats on
baeume-lankow (~5× more content, or the same content at ~55 % of the atlas bytes).

## Choosing the setting

Measure both configurations with identical content and quality settings on the
new layouts. Enable quantization when its measured memory benefit outweighs
its dequantization cost. The historical timing and scene-capacity results above
are not predictions for the new ABI. The `steam_deck` tier still enables the
setting; no current-layout frame-time measurement for that tier is published here.

## Caveats

- **Position/scale bit depth is capped at 16** (the `quantized_position`/`quantized_scale`
  fields are `uint16`). `compression/position_bits` and `compression/scale_bits` accept 8–16 and
  are clamped to that range on load (`QuantizationConfig::load_from_project_settings()`); the
  publisher also caps them at `GS_QUANTIZED_BITS_MAX` (16).
- **Quantized content is lit.** An earlier revision of this page said quantized binning
  zeroes normals, so quantized content was "effectively unlit". **That was fixed in #510
  and the claim is withdrawn.** Normals are carried in the 160-byte payload
  (`normal_xy` / `normal_z_stroke`, written by `pack_gaussian_quantized()` in
  `renderer/gaussian_gpu_layout.cpp`) and decoded in the quantized binning path of
  `shaders/tile_binning.glsl` (`g.normal = LOAD_NORMAL_QUANTIZED(src);`), whose comment names
  the old symptom by issue number. Lighting runs on the shared code after
  the quantized branch closes.
- **SH storage is complete through degree 4 in both layouts.** The strict
  `[SHEncoding]` tests round-trip all 24 non-DC terms and check zero tails and
  padding. Runtime degree/LOD limits still control evaluated terms; this storage
  change alone does not implement SH4 evaluation or antialias compensation, nor
  establish real-scan visual quality.
- Scale quantization is **mandatory** for the quantized layout (it has no unquantized scale
  field); it is forced on internally when quantization is enabled.

## References

- Resident implementation: `renderer/resident_instance_contract_publisher.cpp` (GS-PERF-Q80B).
- Packer: `renderer/gaussian_gpu_layout.cpp` `pack_gaussian_quantized()` (GS-PERF-Q80A).
- GLSL dequant: `shaders/includes/quantization_dequant.glsl`.
