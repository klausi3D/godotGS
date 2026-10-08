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
