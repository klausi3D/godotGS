# Full SH4 selection in resident and streaming rendering

Status: proposed for v1; R3 public rendering semantics, human disposition required.

## Decision

Expose SH4 in project settings and carry degree limits 0–4 through resident chunk
metadata, streaming selection and shader render parameters. Default code quality
becomes SH4 so newly imported degree-four assets are not silently reduced. Existing
explicit settings and named quality tiers retain their recommendations; the
effective configuration reports their source. Lower-degree assets retain their
available-degree cap. GPU records already store all 24 non-DC terms.

Streaming SH reduction is relative to the configured global maximum: at LOD0 use
that maximum, then decrease one degree per LOD when reduction is enabled. Disabling
reduction preserves the maximum. Changing the global maximum invalidates cached
LOD parameter state, even with geometric LOD disabled. Existing standalone
LODConfig calls retain their default maximum 3 for source compatibility; renderer
calls supply the actual configured maximum. Existing splat/VRAM thresholds remain.

SH distribution telemetry gains a fifth entry, preserving existing indices 0–3,
and a degree-four monitor. Report coefficient counts as logical work, not actual
VRAM savings: record allocation is fixed 176/160 bytes regardless of selected degree.
No format change, antialias compensation, HLOD or performance optimization here.

## Validation and release evidence

Native tests cover degree 4 config/counts, old explicit degrees, LOD reductions,
runtime maximum changes with LOD enabled/disabled and five-entry distributions.
Require meaningful clamp/cache mutations, shader matrix compilation, two
independent reviews, uncontended GPU and real-scan comparisons before merge.
No renderer performance or superiority claim follows from structural tests.

## Rollback

Revert selection/config/telemetry together; retain complete imported/stored SH.
Document that the reverted renderer explicitly reduces degree-four assets.

## Selection transport addendum (before implementation)

The normal record has no chunk identifier. StageB already knows the chunk when
emitting each visible reference; carry its current SH degree in the top three
bits of the reference atlas-index word. The low 29 bits remain the physical index.
Checked RD record-byte limits permit fewer than 2^29 normal/quantized records.
StageB rejects indices outside that encoding range before reading/emitting;
binning decodes the physical index and degree for both record permutations.
Sort/gather moves the reference as opaque words and keeps physical-index tie keys.
The eight-byte reference ABI and its allocation size remain unchanged. Host test
fixtures use the shared scalar codec with limit4. This is a GPU-only internal
semantic change: no file/cache payload or public scene API uses this reference.

Synchronize the effective global SH config at the central production streaming
update entry, including early-return paths. This reaches every active system.
ProjectSettings changes still follow the existing config reload lifecycle; this
change does not introduce a new live-setting subscription.

## Partial authored SH qualification (before implementation)

The accepted payload API and resident/streaming snapshots can carry zero, one or
two first-order coefficients alongside higher-order coefficients. The normal
record describes this compact prefix explicitly. The quantized record instead
has a fixed first-order prefix at words 0–2 and higher-order words at 3–23;
binning synthesizes that metadata. Compacting higher-order coefficients into a
missing first-order slot changes the SH basis used for those coefficients.

Keep the quantized 160-byte ABI and its existing shader metadata. Zero missing
first-order slots and store higher-order terms from word 3, with at most 21 terms.
Apply the coefficient limit to retained coefficients, including the supplied
first-order prefix, rather than to the physical span containing zero holes.
Compression metrics count retained coefficients; encoding scans the physical
span and derives its scale only from selected values. The normal compact layout,
file formats, DC encoding and SH normalization remain unchanged.

For a discriminating fixture, first-order counts 0–2 plus eight higher-order
slots derive degree 2. Set only the first higher-order coefficient nonzero. Its
degree-2 basis is even under direction reversal; placing it in a first-order
slot incorrectly produces an odd contribution. Compare actual native packing
and the production basis with an independent Legendre recurrence. Require actual
normal/quantized GPU colour readback in both directions, a mapping-only reverted
mutation, full-prefix controls, logical-limit/metric checks and two independent
reviews. Source or CPU proof does not substitute for GPU colour evidence.

## Registered instance LOD and live SH reload qualification

A registered non-primary chunk is shared by all instances of an asset. Its
metadata therefore cannot carry an individual instance's SH selection. StageB
uses the visible instance's resolved LOD and the effective configured maximum.
Reduce one degree per LOD only when effective geometric LOD and SH reduction
are enabled; otherwise preserve the maximum. Keep the available-degree cap in
existing raster metadata. Do not change shared chunk content, basis, cache layout
or physical atlas indices.

The existing 464-byte depth parameter record carries maximum0–4 in the low three
bits and enabled reduction in bit3 of its previously reserved
cull_frustum_radius.w component, encoded as an exact small numeric float. Host
and shader define the same policy and preserve all offsets, eight-byte visible
references and their 29-bit physical atlas index. The instance lod.y dense
generation field remains unchanged. Streamed overrides take precedence over
global LOD configuration; resident callers use current LOD configuration.

Runtime settings_changed marks the existing full streaming configuration dirty
state and sets one process-wide SH reload latch (`sh_config_reload_if_settings_changed()`).
The latch is consumed before streaming maximum synchronization, including
failed-init early returns, and by the tile raster parameters, because the resident
backend never runs a streaming update. Existing public full reload invokes the
same SH refresh. Clean frames reuse the loaded SH configuration. The full loader
remains behind the existing failed-init guard because its streaming tuning path
can start/stop pack workers; SH refresh neither clears pending full dirty state
nor creates workers or RD resources.

The amortized SH colour cache is keyed per physical Gaussian and shared by every
instance of an asset, so only references at the full configured degree read or
write it; reduced per-instance limits evaluate fresh.

Qualify with real settings signals and no manual global SH reload, initialized
and failed-init paths, retained full-dirty/no-worker early-path controls, normal
and quantized production StageB readback for registered non-primary assets and
shared chunks at different instance LODs. Test expected degree with an independent
explicit table. Require actual old-source RED, corrected GREEN, per-instance
mapping reversion and host-policy wiring deletion, plus existing shader matrix,
independent reviews, full/runtime/GPU evidence and human disposition. No threshold
or format change follows from this correction.
