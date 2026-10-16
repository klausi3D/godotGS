# Spherical harmonics quality

`rendering/gaussian_splatting/rendering/sh_bands` accepts 0–4. The registered
value `-1` selects the named quality tier recommendation, or SH4 when no tier
supplies one. Existing explicit values 0–3 remain intentional quality reductions.
The effective configuration snapshot reports the selected degree and its source.
Degree-four assets require SH4 for all view-dependent terms. Lower-degree assets
use their available terms; raising the maximum does not invent missing data.

Streaming reduction, when enabled, subtracts one degree per geometric LOD from
the configured maximum. Disable SH reduction to retain the maximum at every LOD.
Disabling geometric LOD also retains the configured maximum. Runtime maximum
changes refresh cached chunk metadata. This does not remove the existing spatial
splat-prefix LOD limitation; HLOD remains necessary for the v1 release.

The `sh_band_distribution` statistics array has five entries for SH0–SH4; existing
indices 0–3 keep their meanings. Monitor
`gaussian_splatting/lod_sh_band_4_chunk_count` reports visible chunks at degree 4.
The existing degree3 monitor remains available. These counts describe selected
chunk quality, not proof of GPU execution or full-asset visual correctness.

Selecting fewer degrees reduces evaluated SH work. GPU record allocation remains
176 bytes unquantized or 160 bytes quantized. The legacy logical coefficient ratio
relative to SH3 is not a measurement of allocated VRAM or frame time.

Full SPZ-v4 rendering additionally requires antialias compensation and qualified
GPU/real-scan evidence. SH4 availability alone does not complete that qualification.
