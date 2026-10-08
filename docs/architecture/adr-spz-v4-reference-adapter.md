# ADR: SPZ v4 reference adapter and import data contract

- Status: accepted for implementation under the approved v1 roadmap; merge requires R3 review and maintainer disposition.
- Date: 2026-10-08
- Risk: R3 (third-party decoder, import semantics and cache/API contract)
- Base: `6bacd671a5ba290f363c9b13da2d22e78dc94d47`, stacked on GSF v3 PR #1200.

## Problem

The hand-written SPZ loader does not implement v4. Its color interpretation differs
from Niantic's decoder, quaternion conversion is applied separately from positions
and SH, and antialiased training metadata does not reach the runtime payload.
Hand-written fixture encoding currently mirrors those interpretations instead of
using the real producer. These gaps prevent full SPZ qualification.

## Reference dependency

Vendor Niantic SPZ at immutable commit
[`affd0ecea7fbb4c265ee119475af7ee5b2997482`](https://github.com/nianticlabs/spz/tree/affd0ecea7fbb4c265ee119475af7ee5b2997482),
including its MIT license and source provenance. Compile the C++ decoder and
supported extension implementations directly in the module. Link Godot's existing
Zlib and Zstd; do not run upstream CMake FetchContent or download during a build.
Document any minimal Godot integration patches separately from the pinned source.

The library supplies format decoding, quantization and coordinate/SH transforms.
GodotGS owns preflight size/allocation checks, activated runtime values, caching,
GPU evaluation and antialias raster compensation.

## Runtime meaning and conversion

Niantic's unpacked values contain logarithmic scales, logit opacity, raw SH-DC
coefficients, xyzw quaternions and coefficient-major RGB SH arrays through degree
4. Activate scale and opacity exactly once. Preserve all 24 non-DC RGB
coefficients for SH4 (three embedded SH1 plus 21 sidecar coefficients).

For the existing `GAUSSIAN_DC_ENCODING_LINEAR_RGB` raster route, `Gaussian::sh_dc`
stores the DC **contribution** `C0 * f_dc`, where
`C0 = 0.28209479177387814`; actual view-independent RGB is that contribution plus
0.5. The historical enum name does not imply that stored values already include
the bias. Niantic's byte color contract is decoded by the library, including its
0.5 offset and 0.15 scale. Do not apply that conversion again, clamp the DC before
SH evaluation, or send raw SH-DC coefficients as actual RGB. Keep the legacy
sigmoid route explicit; do not reinterpret previously authored GSF render tags.

Godot uses right/up/back coordinates. Request RUB from the official converter,
including quaternion and all SH-band transforms. Apply no additional ad-hoc Z
flip. Honor supported coordinate-system extensions; reject unsupported packing
semantics explicitly rather than render with guessed coordinates.

The asset's antialias training flag is preserved in `GaussianData`, imported
resource metadata, runtime reconstruction and runtime-to-asset persistence.
Mixed asset raster behavior and filter/opacity compensation are implemented and
qualified in the subsequent renderer task. This adapter does not claim those
rendering features by preserving metadata alone.

## Bounds, errors and legacy compatibility

Retain the established 512 MiB input, 1 GiB decoded-data and 33,554,432-point hard
caps. Use checked wide arithmetic and a machine-derived working-memory budget
before scene-sized library/staging allocations. Validate degree 0-4, supported
versions, fractional bits, flags, reserved bytes, exact attribute extents and
v4 stream/TOC boundaries before entering the decoder. Bound legacy gzip expansion
with Godot's existing decoder; never let an untrusted stream allocate without a
cap. Avoid a second full inflated float cloud when the packed per-point API can
populate runtime storage directly.

Preserve official legacy gzip versions and explicitly distinguish the historical
GodotGS header-plus-gzip wrapper from the official format. Header information and
statistics publish only after a successful complete load. A failed reload leaves
the prior valid resource intact. Bounded malformed input returns an Error; genuine
allocator exhaustion remains subject to the underlying engine/library allocator
contract and must not be reported as qualified without a pressure run.

Compile the library's sequential stream codec path rather than launch unmanaged
`std::async` work from Godot import workers. Keep this integration choice observable
in the dependency record; it is not a renderer timing claim.

## Import/cache contract

Increase the SPZ importer version so cached imports with the former DC/quaternion
contract are regenerated from source. Preserve sidecars, explicit layout and
antialias metadata through imported resource save/load and both resident and
streaming payload construction. Use the same DC contribution meaning in raw
loader and resource reconstruction. Do not mix different asset raster semantics
in a density merge; cross-asset merge admission is a separate focused contract.

## Required evidence

- Real upstream producer output for legacy and v4 fixtures; test SH0-4, nontrivial
  rotations, signed DC, opacity/scale activation and antialias metadata.
- Numeric comparisons with official unpacking and analytic color/activation
  expectations. Coordinate-extension tests must include higher-band coefficients.
- Actual imported-resource roundtrip and runtime reconstruction, including cache
  version checks and failure preserving an already loaded resource.
- Malformed v4 TOC/stream/header/extension cases and legacy expansion limits before
  allocation; retain existing malformed-input acceptance requirements.
- Mutation proof for the DC conversion, SH4 preservation and decoder preflight
  wiring. Two independent read-only reviews of the fixed base..head.
- GPU SH4/reference images and compensated antialiasing are subsequent required
  v1 evidence, not implied by CPU adapter tests.

## Rollback

Revert the focused adapter commits before adoption. Keep the GSF v3 reader for
already saved files. Old SPZ import caches require reimport when changing decoder
contracts; no silent reconstruction from incompatible cache data.
