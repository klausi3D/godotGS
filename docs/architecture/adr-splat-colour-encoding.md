# ADR: Explicit, signed splat colour encoding (SH sign + DC contract) (#1054, #1056)

- **Status:** **Proposed**. This is a recommendation awaiting maintainer approval. Nothing
  here is accepted until a maintainer says so, and no implementation PR may land before
  that.
- **Risk class:** this document is R0 (`docs/**`). The change it governs is **R3**
  (persistence/on-disk formats, `docs/governance/agentic-engineering.md:111`), which is why
  an ADR comes before any code. §7 gives the class of each implementation slice.
- **Tracking:** #1054 (audit `GS-AUDIT-SHD-002`, P1) and #1056 (`GS-AUDIT-PERS-010`, P1).
  §8 sets the scope for the related records `GS-AUDIT-PERS-011` and `GS-AUDIT-PERS-016`.
- **Verified against:** `origin/master` = **`0fcefee1779`**. Every `file:line` below was
  read at that commit. Module paths are relative to `modules/gaussian_splatting/`.
- **Date:** 2026-09-23

## 1. Context

Both defects come from one missing contract: nothing defines how a splat's colour is
stored between the loader and the shader.

### 1.1 Signed SH is stored in an unsigned format (#1054)

`encode_rgb9e5` clamps each channel to `[0, 65408]` (`renderer/gaussian_gpu_layout.cpp:26-30`).
It is the only SH encoder. It encodes band 1 and the higher bands in the unquantized packer
(`:120`, `:129`) and in the quantized packer (`:257`, `:264`). The decoder has no sign
either (`shaders/includes/gs_sh_binning.glsl:41-49`). Trained SH coefficients have zero mean,
so about half of them are set to 0. The audit probe on 2026-09-23 found that negative
band-1 SH renders bit-identical to zero SH (R 0.4986 against 0.4986), while positive SH
renders R 0.6037.

Nothing on disk is lossy here. Every persisted format stores SH as signed `float`: the
authoring struct (`core/gaussian_data.h:168-169`), the high-order sidecar in `.gsplatworld`
(`io/gaussian_splat_world_io.cpp:303`) and the imported asset arrays
(`core/gaussian_splat_asset.h:92-94`). **#1054 is a GPU-layout defect only.**

### 1.2 GPU layouts today (bytes per splat)

| Layout | Size | SH storage | Source |
| --- | --- | --- | --- |
| `Gaussian` (CPU authoring, raw on disk) | 144 B | `Color sh_dc` + `Vector3 sh_1[3]`; higher bands in a float sidecar | `core/gaussian_data.h:158-196` |
| `PackedGaussian` (unquantized GPU) | **128 B** | `float dc[4]` + 12 × u32 RGB9E5 words | `renderer/gaussian_gpu_layout.h:238-274` |
| `PackedGaussianQuantized` (GPU) | **80 B** | `float sh_dc[4]` + 6 × u32 RGB9E5 words | `renderer/gaussian_gpu_layout.h:299-320` |

The figure of 144 B/splat that circulates in older notes is the size of the *authoring*
struct. The GPU atlas stride is 128 B or 80 B (`core/gaussian_streaming.cpp:3582-3591`).
Several comments still say 144 B, for example `core/gaussian_streaming.cpp:1523`. At 5M
splats the SH-bearing atlas is 640 MB unquantized and 400 MB quantized.

Capacity limits that already exist: the unquantized layout holds 3 band-1 coefficients and
at most 9 higher-band coefficients (`gaussian_gpu_layout.cpp:114-126`), so SH degree 3
loses 3 of its 7 band-3 coefficients. The quantized layout holds 6 coefficients in total
(`:184-188`, `:252-264`).

**Every GPU path packs through two functions.** `pack_gaussian` (`gaussian_gpu_layout.cpp:72`)
and `pack_gaussian_quantized` (`:190`) are called, through their range wrappers, from the
following sites:

- resident `GaussianData` buffers: `core/gaussian_data_gpu.cpp:192`, `:271`;
  `renderer/gpu_buffer_manager.cpp:683`; `renderer/gpu_memory_stream.cpp:429`, `:440`;
- the resident instance atlas: `renderer/resident_instance_contract_publisher.cpp:671`
  (quantized) and `:700`;
- streaming chunks: `core/gaussian_streaming.cpp:3723` (quantized) and `:3734`;
  `core/streaming_upload_pipeline.cpp:1291`.

No packed GPU payload is ever written to disk. Streaming reads `Gaussian` structs from the
`.gsplatworld` and packs them at runtime (`core/streaming_chunk_payload_source.h:81-85`).

### 1.3 The DC encoding is a per-splat bit whose zero means "legacy" (#1056)

`GaussianDCEncoding` has two values, `LEGACY_BIAS = 0` and `LINEAR_RGB = 1`
(`core/gaussian_data.h:131-134`), stored in bit 0 of `Gaussian::render_meta`, which
defaults to 0 (`:183`). The packer copies the bit into bit 31 of `sh_metadata`
(`renderer/gaussian_gpu_layout.h:19`, `:23-36`; `gaussian_gpu_layout.cpp:148-153`). The
quantized path instead takes it from an asset flag (`gaussian_gpu_layout.h:48-54`;
`shaders/tile_binning.glsl:737`). The shader decodes `LINEAR_RGB` as `sh_dc + 0.5` and
`LEGACY_BIAS` as `1.5*sigmoid(sh_dc) - 0.25` (`gs_sh_binning.glsl:140-147`).

The code falls back to **four different defaults** when no tag is present:

| Where | Untagged or empty resolves to | Source |
| --- | --- | --- |
| struct, packer, GPU bit | `LEGACY_BIAS` | `core/gaussian_data.h:143-146`, `renderer/gaussian_gpu_layout.h:42-46` |
| asset metadata (missing **or unknown** string) | `LINEAR_RGB` | `core/gaussian_splat_asset.cpp:28-36`, `core/gaussian_data_io.cpp:84-92`, `core/gaussian_splat_merge_utils.cpp:26-41` |
| resident asset flag, empty data | `LEGACY_BIAS` | `renderer/resident_instance_contract_publisher.cpp:75-79` |
| streaming asset flag, empty data | `LINEAR_RGB` | `core/streaming_global_atlas_registry.cpp:34-38` |

Both asset-flag resolvers read **splat 0 only**, even though the tag is stored per splat.

### 1.4 Producers, enumerated from the code

| Producer | What it stores in `sh_dc` | Tag it sets | Source |
| --- | --- | --- | --- |
| `PLYLoader` (all PLY paths) | `SH_C0 * f_dc` | **none, so 0 (legacy)** | `io/ply_loader.cpp:1094-1105` |
| PLY importer | loader data | metadata `"linear_rgb"`, applied at load | `io/resource_importer_ply.cpp:558`, `:618`; `core/gaussian_splat_asset.cpp:1764-1768`; `core/gaussian_data_io.cpp:280-284` |
| `GaussianSplatAsset.load_from_file(.ply)` | loader data, cached as is | none. Metadata is then *derived* as `"legacy_bias"` | `core/gaussian_splat_asset.cpp:1416-1426`, `:1511`, `:2059-2071`, `:2187-2193` |
| `GaussianData.load_from_file(.ply)` | loader data | none | `io/gaussian_data_loader.cpp:42-58` |
| `.gsplatcache` (PLY sidecar) | loader data, written as a world file | none | `io/ply_loader.cpp:465-494` |
| `SPZLoader` | `byte / 255`, treated as display colour | `LINEAR_RGB` | `io/spz_loader.cpp:630-643`, `:381-382` |
| SPZ importer | loader data | metadata `"linear_rgb"` | `io/resource_importer_spz.cpp:521` |
| `.gsplatworld` / GSF | raw `Gaussian` bytes, including `render_meta` | whatever was saved | `io/gaussian_splat_world_io.cpp:302`, `:383-397`; `persistence/gaussian_scene_serializer.cpp:535-543`, `:819` |
| GSIF incremental deltas | `sh_dc` values only. A `render_meta` change forces a full save | n/a | `persistence/incremental_saver.cpp:47-75`, `:328-335` |
| merge | per-source tag taken from metadata | per splat | `core/gaussian_splat_merge_utils.cpp:367`, `:413` |
| `GaussianData.resize` | `(1,1,1,1)` | none | `core/gaussian_data.cpp:537-548` |
| `GaussianSplatNode3D.set_splat_data(colors)` | display colour | none | `nodes/gaussian_splat_node_3d.cpp:984-999` |
| brush, grading bake, animated colour (PERS-016) | treat `sh_dc` as display RGB, and grading clamps it with `MAX(0)` | unchanged | `core/gaussian_data_edits.cpp:199-203`, `:317-321`; `core/gaussian_data_color_grading.cpp:31-45`, `:99-101`; `core/gaussian_data_animation.cpp:142` |
| PLY export (`GaussianData.save_to_file`) | writes `sh_dc / SH_C0` | ignores the tag | `core/gaussian_data_io.cpp:380-382` |

This table is the root of #1056. The runtime probe rendered the same PLY at 0.779 through
the importer and at 0.600 through `load_from_file`. The first value matches the reference
decode `0.5 + SH_C0`. The second matches the legacy decode applied to C0-prebaked data.
SPZ stores `byte / 255` and tags it linear, so the shader adds another +0.5. That half has
only static evidence so far.

**All 11 tracked world-format fixtures are untagged.** Nine `.gsplatcache` files and two
`.gsplatworld` files under `tests/fixtures/` and `tests/examples/godot/test_project/tests/fixtures/`
have `render_meta & 1 == 0` on every splat. This was read from the files at the base
commit (header offset 104, struct offset 132). Their DC values look authored as display
colours: `test_splats.gsplatworld` splat 0 has `sh_dc = (1.0, 0.1, 0.1)`.

## 2. Options for signed SH storage

All sizes below are per coefficient triplet and per splat for the current slot counts:
12 unquantized, 6 quantized. Error is the worst-case absolute error per channel after
round-to-nearest. `m` is the triplet's largest channel and `s` is the splat's largest
stored coefficient magnitude. Decode cost has **not been measured** for any option. Each
is a handful of ALU operations per coefficient in binning, and the SH colour cache
amortises it further (`tile_binning.glsl:1228-1247`). Slice 1 must measure it.

| Option | Bits / triplet | Δ B/splat (unq. / quant.) | Δ VRAM at 5M | Max error | Quantized path | Shader change |
| --- | --- | --- | --- | --- | --- | --- |
| today: RGB9E5, unsigned | 32 | 0 / 0 | 0 | `m/512`, **negatives lost** | yes | n/a |
| **A.** signed shared exponent (3 × (sign + 8-bit mantissa) + 5-bit exponent) | 32 | 0 / 0 | 0 | `m/256` | fits the 6 slots as is | new decoder, sign bits |
| **B.** bias: RGB9E5 of `c + b` | 32 | 0 / 0 | 0 | about `b/512` for small `c`; still clamps `c < -b` | fits | subtract `b` |
| **C.** half-float per channel | 48 | +32 / +16 (160 B / 96 B after 16-B alignment) | +160 MB / +80 MB | `abs(c)·2^-11` | layout change, 80 → 96 B | `unpackHalf2x16`, struct rework on both sides |
| **D.** SNORM10 with a per-asset or per-chunk scale | 32 | 0 / 0 (+ per-asset or per-chunk meta) | ~0 | `scale/1022`, and the scale is set by the asset's outliers | chunk id exists only on the quantized path | scale lookup through asset or chunk meta |
| **E.** SNORM10 with a **per-splat** scale in the unused `sh_dc.w` lane | 32 (+2 spare bits) | **0 / 0** | **0** | `s/1022` | fits: `sh_dc[4]` is FP32 on both layouts | signed bit-extract × `sh_dc.w` |

Notes on each option:

- **B** is rejected. It keeps a clamp, which is the defect class being fixed. It also has
  its worst precision on the small higher-band coefficients.
- **D** is rejected. The scale has to be known before the first streaming chunk is packed,
  and chunks are packed independently (`core/gaussian_streaming.cpp:3723-3741`). That needs
  either a whole-asset pre-pass or a persisted scale, which means a format bump. The
  unquantized layout has no chunk id, so the scale would be per asset. One outlier would
  then set the step size for the whole asset, or the outliers would be clamped.
- **C** is the most precise, but it costs +25 % unquantized and +20 % quantized atlas
  VRAM and bandwidth. It also rewrites both struct mirrors and the quantized dequant
  (`shaders/includes/quantization_dequant.glsl:37-53`).
- **A** and **E** both cost zero bytes and need no layout change. A's error scales with
  each triplet's own maximum. E's error is uniform in absolute terms within a splat, and
  absolute error is what reaches the pixel. The basis constants are at most 2.89
  (`gs_sh_binning.glsl:52-60`), so E's per-coefficient colour error is at most
  `2.89·s/1022 ≈ 0.0028·s`. That is below one 8-bit step for `s ≤ 1.38`. E also removes
  the per-coefficient `log`/`pow` from the host encoder (`gaussian_gpu_layout.cpp:37-51`).
  **E's lane claim:** no shader reads `sh_dc.w`. A grep of `shaders/` at the base commit
  finds only `.rgb` reads (`gs_sh_binning.glsl:143`, `:146`) and whole-`vec4` copies
  (`tile_binning.glsl:736`, `quantization_dequant.glsl:189`). The host writes `sh_dc.a`
  into that lane today (`gaussian_gpu_layout.cpp:112`, `:244`). Slice 1 must prove the
  claim again at its own head.

For A and E, the all-zero word still decodes to zero, so zeroed unused slots keep working
(`gaussian_gpu_layout.cpp:184-188`). Both need a new SH encoding id in the 7-bit metadata
field (`gaussian_gpu_layout.h:18`, `:20`). The quantized metadata synthesiser hard-codes
the RGB9E5 id (`tile_binning.glsl:443-451`) and must change in the same commit. The shader
currently falls back silently to DC-only on an unknown id (`gs_sh_binning.glsl:156`). That
fallback must become observable, for example as a debug counter, because
`modules/gaussian_splatting/AGENTS.md` forbids silent fallbacks.

There is also a second RGB9E5 decoder in `shaders/includes/gaussian_splat_common_inc.glsl`,
at line 207, with evaluation at `:306-311`. It belongs to `gaussian_splat.glsl` (`:6`,
`:80`), which is compiled at `renderer/render_resource_orchestrator.cpp:56-93`. Its DC
path reads `sh_dc` raw, with no +0.5. **Whether it is reachable in production was not
traced.** Slice 1 must either change it in lockstep or prove it is dead and delete it.

## 3. The DC encoding contract

1. **One canonical in-memory encoding.** `sh_dc.rgb = SH_C0 · f_dc`, and the linear
   display colour is `sh_dc.rgb + 0.5`. This is the Inria `SH2RGB`, and it is what the
   PLY loader already produces (`io/ply_loader.cpp:1096-1102`). The stale comment at
   `:1092` ("We add 0.5 here") is corrected.
2. **The tag is explicit and has no default.** The enum is redefined as
   `GAUSSIAN_DC_ENCODING_UNSET = 0`, which is invalid, and `GAUSSIAN_DC_ENCODING_SH_C0 = 1`.
   `SH_C0` keeps the numeric value of today's `LINEAR_RGB`, so every splat already tagged 1
   on disk keeps its meaning, and only 0 changes meaning. A zero tag is rejected
   everywhere: by loaders and readers with a user-facing error naming the file, and by
   both packers as defence in depth, where the chunk fails and nothing is uploaded. The
   `void` range packers (`gaussian_gpu_layout.cpp:278`, `:363`, `:387`) become fallible.
3. **The serialized metadata token stays `"linear_rgb"`.** It is documented as a
   historical name for `SH_C0`. Keeping it avoids reimporting every PLY asset for a
   rename. The resolvers stop defaulting: a missing key, `"legacy_bias"` or any unknown
   string is rejected with a re-import message. Today all of them become `LINEAR_RGB`
   (`core/gaussian_splat_asset.cpp:28-36` and its two copies).
4. **Every producer tags at the source.** Tagging happens in the loader, so every consumer
   inherits it:

   | Producer | Emits |
   | --- | --- |
   | `PLYLoader` | `SH_C0·f_dc`, tagged `SH_C0`. This covers the importer, both raw `load_from_file` paths and `.gsplatcache` |
   | `SPZLoader` | converted from `byte` to `SH_C0 · (byte/255 − 0.5) / 0.15`, tagged `SH_C0`. The 0.15 `colorScale` comes from the upstream `nianticlabs/spz` reference and **is not verified in this ADR** (§6) |
   | `set_splat_data(colors)` | display colour converted: `sh_dc = color − 0.5`, tagged |
   | `GaussianData.resize` | white becomes `sh_dc = (0.5, 0.5, 0.5)`, tagged |
   | `set_spherical_harmonics` | documented as `SH_C0` coefficients, tagged |
   | merge | requires tagged inputs, so the per-source tag lookup disappears |
   | `.gsplatworld` / GSF readers | validate that every splat is tagged; any zero fails closed |
   | CPU edit, bake and export paths | work in display space through two helpers, `gaussian_dc_to_display()` and `gaussian_display_to_dc()`, defined next to the enum |

5. **`LEGACY_BIAS` does not survive**, for three reasons. (a) No in-tree producer emits
   data in sigmoid space. Every splat tagged 0 today is either untagged C0-prebaked PLY
   data (§1.4) or comes from a pre-v4 import that already had to be reimported
   (`io/resource_importer_ply.h:45-49`). (b) Honouring the tag reproduces #1056 exactly.
   (c) Keeping it costs the dual decode, the DC bit in `sh_metadata`, the asset flag, both
   splat-0 resolvers, and the quantization DC-compatibility gate along with its
   stride-flip eviction logic (`core/gaussian_streaming.cpp:93-104`, `:1508-1530`). A
   future source whose data really is in sigmoid space must convert at its loader.

## 4. Versioning and migration

The alpha makes no stability promise for on-disk formats (`docs/development/api-stability.md:3`,
`:15-16`). Re-importing from source is the supported way to migrate (`:42-43`). The module
rule still applies, though: a format bump keeps read paths able to load the previous
version and validates on load (`modules/gaussian_splatting/AGENTS.md:37-39`). **No path may
silently misread.** Each artifact either keeps its meaning, gets re-derived from source, or
fails closed with a message naming the file.

| Artifact | Constant at base | Decision | Existing files |
| --- | --- | --- | --- |
| `.gsplatcache` (PLY sidecar) | `PLY_CACHE_VERSION = 3` (`io/ply_loader.cpp:35`) | **3 → 4**. The loader's decode semantics change, which is exactly the bump rule at `:16-19` | rejected by `_cache_version_matches` (`:122-127`); the raw PLY is re-parsed automatically |
| SPZ import (`.res`) | `get_format_version() = 8` (`io/resource_importer_spz.h:61`) | **8 → 9**. The importer's output DC values change | Godot re-imports on the version mismatch (`io/resource_importer_ply.h:30-34`) |
| `.gsplatworld` import | `get_format_version() = 2` (`io/resource_importer_gsplatworld.h:27`) | **2 → 3**, so the decode check at import (`io/resource_importer_gsplatworld.cpp:347`) runs again and reports untagged worlds in the editor instead of at scene load | re-imported; untagged worlds fail with a message |
| PLY import (`.res`) | `get_format_version() = 11` (`io/resource_importer_ply.h:103`) | **no bump**. The output bytes and the `"linear_rgb"` token are unchanged, and a bump would reimport every PLY for no data change | unaffected |
| `.gsplatworld` container | `kWorldVersion = 1`, strict equality (`io/gaussian_splat_world_io.cpp:24`, `:532-538`) | **no bump**. The layout is unchanged and tag 1 keeps its meaning; content validation on read rejects tag 0 | tagged files load; untagged files fail closed (`ERR_FILE_CORRUPT` + message) |
| GSF scene | `GAUSSIAN_SCENE_VERSION = 2`, min reader 1 (`persistence/gaussian_scene_serializer.h:17`, `:20`) | **no bump**, same reasoning | same as `.gsplatworld` |
| GSIF incremental | `INCREMENTAL_VERSION = 1` (`persistence/incremental_saver.h:46`) | **no bump**. Deltas carry `sh_dc` in the single canonical space, and the baseline carries the tag | a delta over a rejected baseline is rejected with it |
| GPU layouts | SH encoding id 1 (`gaussian_gpu_layout.h:20`) | new id. GPU layouts are never persisted | n/a. The SPIR-V disk cache is keyed on source text (`renderer/spirv_disk_cache.cpp:222-241`) |

**Alternative considered and rejected:** bumping `kWorldVersion` and GSF. The world reader
uses strict equality, so a bump would reject correctly tagged v1 files unless the reader
were relaxed to accept both versions. That leaves a version that changes nothing about how
a file is read.

**Importer-bump rule.** An importer bump is required whenever the importer's *output*
changes. Each bump adds a `vN:` line to the history comment
(`io/resource_importer_ply.h:30-100`; the SPZ importer points to it at
`io/resource_importer_spz.h:39`). If the PERS-011 fix (§8) lands in the same release, it
shares the `PLY_CACHE_VERSION` bump and adds the PLY importer bump that it needs anyway,
so users re-import only once.

**Fixtures:** the 11 untagged fixtures (§1.4) are regenerated in the slice that adds read
validation. Because their DC values look like display colours, regeneration has to decide
each fixture's *intended* colour, not just re-tag it. Tests that encode the current
contract are **replaced, not weakened**:

- `tests/test_gaussian_importer.h:1902-1929`
- `tests/test_gaussian_splat_container.h:160-172`, mixed-tag merge
- `tests/test_gpu_streaming.h:64-72`, mixed-tag streaming
- `tests/test_quantized_packing.h:164-179`, positive coefficients only

## 5. Recommendation (awaiting maintainer approval)

**Recommended: Option E for SH, plus the explicit DC contract in §3, with `LEGACY_BIAS`
removed.**

- **Bytes per splat: +0** (128 B / 80 B unchanged). **VRAM at 5M: +0.**
- No format bumps for SH, because the change is GPU-only. The DC contract bumps
  `PLY_CACHE_VERSION` (3 → 4), the SPZ importer (8 → 9) and the `.gsplatworld` importer
  (2 → 3).
- **Pre-agreed escalation:** if the real-scan check (§6) shows error attributable to
  per-splat scaling, switch the SH slice to **Option C**, accepting +32/+16 B per splat.
  Option A is the fallback if the maintainer would rather keep each coefficient word
  self-contained.

## 6. Acceptance evidence required from the implementation PRs

Every new test must **fail on the base commit** before the fix, shown by building and
running it on the base, and pass after. A test that cannot fail is not evidence.

1. **Host round-trip doctests** (GPU-free) for `pack_gaussian` and `pack_gaussian_quantized`.
   Inputs: negative, positive, mixed-sign, tiny (1e-4), zero and large (±4)
   coefficients. Each is decoded through a C++ mirror of the GLSL decoder and checked
   against the stated error bound. The layout-sync guard must pin the mirror to the GLSL.
2. **Permanent one-splat GPU readback, negative SH** (RequiresGPU tier). An off-axis
   camera renders one opaque splat three times: with negative, zero and positive band-1
   R. The test asserts that negative < zero < positive, and that the negative delta is
   within tolerance of the analytic value. It runs for both the unquantized and quantized
   atlas and for the resident and streaming routes.
3. **Permanent one-splat GPU readback, DC decode on every producer path.** Splats with
   `f_dc = ±1` must render at `0.5 ± SH_C0` through the PLY importer, both raw
   `load_from_file` paths, `.gsplatcache`, a `.gsplatworld` round-trip, a GSF round-trip,
   and `set_splat_data`. SPZ, through both the importer and a raw load, must match the
   reference decoder's output for an **official Niantic sample**, not only the in-repo
   synthetic writer, which is the loader's own inverse.
4. **Cache and format migration tests:**
   - a v3 `.gsplatcache` is rejected and re-derived as tagged;
   - a tagged v1 `.gsplatworld` and a tagged v2 GSF load;
   - an untagged one fails closed with the file named, and nothing reaches the renderer;
   - `"legacy_bias"`, missing and unknown metadata tokens are each rejected.
5. **Real-scan visual check**, as `tests/AGENTS.md:23-24` requires for rendering-math
   changes. Render at least one SH-degree-3 real scan from fixed on-axis and off-axis
   cameras, before and after, next to a reference 3DGS renderer. Report the difference
   metrics and get human sign-off. Record the vendor coverage as a blind spot if only one
   vendor was tested.
6. **Cost evidence:** the static size asserts stay unchanged (`gaussian_gpu_layout.h:266`,
   `:319`). A binning-pass GPU timing A/B against the base uses the pass-timing surface,
   with `*_valid` asserted.

## 7. Implementation slicing

Each slice is one PR that references this ADR and states its base SHA. They land in this
order.

| # | Slice | Class | Depends on |
| --- | --- | --- | --- |
| 1 | Signed SH encoder and decoders (both layouts, both GLSL decoders, metadata id, observable unknown-id fallback), with evidence items 1, 2, 5 and 6 | **R2** | ADR approval |
| 2 | DC contract core: enum with `UNSET = 0`; tagging in `PLYLoader`, `resize` and `set_splat_data`; non-defaulting resolvers; read validation for world and GSF; fallible packers; `PLY_CACHE_VERSION` and `.gsplatworld` importer bumps; fixture regeneration; evidence 3 (PLY rows) and 4 | **R3** | 1 is not required but reduces visual confounds |
| 3 | SPZ DC decode and SPZ importer bump; evidence 3 (SPZ rows) | **R3** | 2 |
| 4 | Remove the `LEGACY_BIAS` decode, the `sh_metadata` DC bit, the asset flag, the splat-0 resolvers and the quantization DC-compatibility gate | **R2** | 2, 3 |
| 5 | CPU consumers (PERS-016): brush, grading bake (no coefficient-space `MAX(0)`), animated colour, PLY export, all through the display helpers | **R1 by path**; evidence handled as R2 because it changes rendered colour | 2 |

## 8. Non-goals and risks

**Non-goals:**

- **`GS-AUDIT-PERS-011`** (PLY `f_rest` channel stride fixed at 15 for degree 1 and 2,
  `io/ply_loader.cpp:1109`). This is an importer decode bug, not a storage-contract
  question, and gets its own issue and PR. It only has to coordinate version bumps (§4).
- SH capacity truncation: 12 and 6 slots, so band 3 is partly dropped (§1.2).
- SPZ decode beyond DC colour, meaning the scale, alpha and rotation discrepancies noted
  in PERS-010.
- GSF not persisting high-order SH (`persistence/gaussian_scene_serializer.cpp:805-809`).
- PLY export dropping `f_rest`.
- Output colour management.
- Any VRAM reduction.

**In scope, as slice 5:** `GS-AUDIT-PERS-016`. It consumes this contract, and the helpers
it needs are defined here.

**Risks:**

- **Visible changes.** Raw-loaded PLYs gain contrast, and they become correct. SPZ assets
  change substantially. Scenes tuned against the wrong decode will look different, and
  the release notes must say so.
- **Fail-closed rejections.** User `.gsplatworld` and GSF files saved from raw-loaded data
  are untagged. They stop loading until they are regenerated from source. An explicit
  re-tag tool is left out on purpose, because a re-tag cannot tell C0-prebaked data from
  display-colour data (§1.4 fixtures).
- **Option E precision.** A splat with one outlier coefficient gets coarser steps for its
  other coefficients. Slice 1 must report the distribution of `s` on the real scan, and
  the escalation in §5 applies.
- **The SPZ constant is external and unverified.** Slice 3 must cite the upstream source
  line and test against an official sample.
- **Single-vendor evidence.** The audit probes ran on one NVIDIA RTX 3090 with a `-O0`
  build. Any other vendor that was not tested is a recorded blind spot.
