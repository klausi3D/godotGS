# Niantic SPZ dependency

Pinned upstream: https://github.com/nianticlabs/spz/tree/affd0ecea7fbb4c265ee119475af7ee5b2997482

License: MIT, retained in LICENSE and source headers. Original file hashes and
all Godot integration patches are listed in UPSTREAM.json. Decoder/type sources
come from src/cc; extension sources come from extensions/cc at that same commit.
No upstream build scripts or downloaded compression libraries are executed.

SCons compiles the library separately with extension support, sequential codec
work and local RTTI support for the official extension registry. The surrounding
Godot module retains its normal compiler settings. Zlib and Zstd headers and
symbols come from the existing engine dependencies.

The adapter validates bounded legacy/v4 envelopes before scene-sized allocation,
uses the official packed decoder and per-point unpack/coordinate conversion, and
activates runtime scale/opacity/DC exactly once. Its bounded legacy stream entry
point is declared in the adapter against this pinned implementation.
