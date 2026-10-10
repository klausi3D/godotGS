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

## GSF v3 wire contract (2026-10-08)

The full-save writer emits GSF version 3 with minimum reader version 3. The
60-byte scene header and 16-byte chunk headers remain unchanged. Every v3 scene,
including an empty scene, has exactly one GAUSSIAN_DATA chunk. Legacy v1/v2
GAUSSIAN_DATA keeps its original count plus raw 144-byte record decoder.

The v3 decoded chunk starts with eight little-endian uint32 fields (32 bytes):
count, schema (=1), record size (=132), SH degree, first-order count, high-order
count, semantic flags (bit 0: 2D; bit 1: antialiased), reserved (=0). It is followed
by count canonical records, then count * high-order-count RGB float32 vectors.
A record explicitly stores position, opacity, scale, area, quaternion xyzw, DC
rgba, three first-order RGB vectors, normal, stroke age, brush axes, painterly
metadata and render metadata in that order. Padding is not persisted. Existing
per-record DC encoding is retained; this task does not reinterpret color.

SH degree is 0--4, first-order count is 0--3 and high-order count is 0--21.
The declared degree agrees with the existing authoring layout's degree derivation;
partial legacy layouts remain representable. Full SH0--4 layouts retain all
1/4/9/16/25 RGB vectors. Unknown semantic bits, schema/element-size mismatches,
nonzero reserved data, duplicate/missing payloads and non-exact lengths fail
before target mutation. Memory bounds and codec/checksum protections apply to
both uncompressed and compressed payloads. Size arithmetic uses uint64 before
checking the uint32 wire limit. Real allocation failure remains subject to the
engine LocalVector container's existing fatal-OOM contract; checked byte-buffer
allocations return errors and leave the previous target intact.

Antialias semantics become an asset-level field captured under data_rwlock and
committed together with geometry/SH/2D state. This is preservation metadata; the
subsequent SPZ/raster task implements filter and opacity compensation. Existing
callers of the bulk payload setter default to classic semantics. The incremental
baseline uses this same v3 writer; a semantic or SH-layout edit still requires a
full baseline, while supported per-index deltas preserve baseline sidecars.

Acceptance adds legacy v1/v2 loading, SH0--4 complete roundtrips for every codec,
empty semantic datasets, incremental baseline + delta retention, semantic-edit
full-save requirements, malformed metadata and transactional rejection. Tests
that encode the previous known loss are replaced by preservation assertions;
malformed fixtures continue to test their original failure property.
