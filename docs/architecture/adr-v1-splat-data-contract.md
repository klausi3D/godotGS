# ADR: v1 splat payload, snapshots and lossless persistence

- Status: accepted for implementation by the complete v1 implementation request, 2026-10-06.
- Risk: R3 (format and compatibility).
- Base: `ab74e332aa8264ab6d0bfb2dc45ee83778a9e293`.
- Related: issue #600 and [accepted HLOD design](adr-hlod-streaming.md).

## Authoritative payload

An asset's payload consists of authoring geometry, opacity, DC encoding, all SH
coefficients, 2D semantics, antialias training semantics and its content revision.
A degree `l` requires `(l + 1)^2` RGB coefficient vectors including DC: SH0--4
have 1, 4, 9, 16 and 25 vectors. In the current authoring representation DC and
three first-order vectors are embedded in `Gaussian`; SH4 therefore requires 21
additional vectors. GPU packing must never silently discard them. A lower render
quality is an explicit, observable runtime choice, not a destructive import.

SH DC coefficients, `C0 * coefficient` and display RGB are different quantities.
For coefficient data, base RGB is `0.5 + C0 * coefficient`; RGB-tagged data already
contains the base color. Import performs each source-to-authoring conversion
once. PLY, official SPZ decoding, caches, bake, merge, save and GPU packing use
the same convention. SH direction changes transform every band consistently.
Different 2D/antialias semantics may coexist through per-instance or per-splat
metadata; a merge that cannot preserve them fails explicitly.

## Snapshot boundary

Full save captures one owned payload snapshot under `GaussianData::data_rwlock`.
Geometry, SH sidecars, SH counts/degree, 2D mode, revision and geometry bounds
must describe the same generation. Compression and all file I/O happen after
unlock. The snapshot exposes const accessors; later edits never change its bytes.
Allocation failure returns an error before any target file is replaced. Save
retains the existing atomic temporary-file replacement contract.

The first implementation closes the unsafe raw storage read while retaining the
v2 wire format and its explicit loss warning. It does not claim that v2 becomes
lossless. Animation and serializer configuration are separate objects: callers
still serialize changes to them; this payload lock does not protect those objects.
The subsequent format task uses this snapshot rather than taking additional live
reads. Baseline establishment still checks the captured revision before clearing
incremental tracking, preserving edits that occur after snapshot capture.

## Complete format and cache migration

GSF v3 adds a complete payload schema with explicit element sizes, SH layout,
semantic flags and required-reader version 3. Existing v1/v2 remain readable; a
v3 file cannot advertise v1 compatibility while losing mandatory SH or semantics.
New readers reject duplicate mandatory chunks, missing required payload metadata,
invalid coefficient counts, unsupported semantic bits and inconsistent lengths
before allocation or committing data. Size products use checked wide arithmetic.
Checksums and allocation/decompression limits remain enforced. Full and
incremental save share this payload contract; unsupported deltas require a full
baseline rather than discarding changes. No raw compiler struct is a new wire ABI.

Importer/cache and world payload layout versions increase when the representation
changes. Legacy data is interpreted using its original encoding; it is not
silently re-tagged. The HLOD format receives SH4 and antialias metadata before
its runtime cut, fallback and transition stages are implemented.

## Official SPZ adapter and raster semantics

Vendor the official Niantic source at an immutable commit with license and source
hashes; use Godot's existing compression dependencies, with no build downloads.
The adapter enforces input, output and allocation bounds before decompression and
conversion. Golden assets verify legacy versions and v4; SH0--4 and mixed classic /
antialiased scenes need numeric and GPU image references. The library decodes the
format; GodotGS owns SH evaluation and antialias filter/opacity compensation.

## Acceptance and rollout

1. Snapshot retention and concurrent structural replacement produce a consistent
   payload or an explicit error, never header/data disagreement or invalid memory.
2. v1/v2 fixtures load unchanged; complete v3 roundtrips preserve every supported
   coefficient and semantic field for full and incremental paths.
3. Official SPZ fixtures and real images agree with the reference conversion and
   raster semantics, including SH4 and antialias compensation.
4. Existing HLOD budgets and visual acceptance thresholds remain unchanged.

Each implementation is a focused task/PR with an immutable base. R3 requires two
independent reviews and human/CODEOWNER disposition before merge. Rollback is a
focused revert before new-format files are distributed; once distributed, keep
the new read path or explicitly migrate files before reverting the writer.
