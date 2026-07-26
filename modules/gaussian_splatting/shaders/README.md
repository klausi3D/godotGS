# Gaussian Shader Validation

Canonical validation command (full runtime matrix + contracts):

```bash
python3 modules/gaussian_splatting/shaders/compile_shaders.py \
  --output-dir /tmp/gaussian_shader_validation/spv \
  --summary-json /tmp/gaussian_shader_validation/summary.json
```

Contract-only validation (no SPIR-V compile):

```bash
python3 modules/gaussian_splatting/shaders/compile_shaders.py --contracts-only --list-matrix
```

## Validation Scope

- Runtime stage matrix coverage: `#1267`, `#1318`
- Shader/host ABI contracts: `#1320`
- Per-dispatch counter init contracts: `#1322`
- Diagnostics toggle contracts: `#1324`
- Embedded sorter shader coverage: `#525`

### Embedded sorter shaders (`#525`)

The `RUNTIME_SHADER_MATRIX` above only reaches file-based `.glsl` sources (those a
C++ TU pulls in via a `*.glsl.gen.h` include). The GPU sort path instead builds its
compute shaders as runtime `vformat()` strings, so they are invisible to that matrix.
`compile_shaders.py` closes the gap by **extracting the shader templates straight out
of the live C++ sources** — `renderer/gpu_sorter.cpp` (radix histogram / wg-prefix /
bin-prefix / scatter, bitonic, the indirect-dispatch args shader, and the OneSweep
passes) and `interfaces/gpu_sorting_pipeline.cpp` (remap, gather) — reproducing the
exact `vformat()` substitution and compiling the assembled permutations through the
same compiler path as the file matrix. **Every interpolated piece is sourced from the
C++, never hand-copied**: the shader templates and the small interpolated helper
fragments (subgroup extension block, per-key read snippets, the `uvec2`/`uint` key
type) are extracted from `gpu_sorter.cpp`, and the substituted scalar constants
(`kSortWorkgroupSize` from `renderer/sorting_contract.h`; `DEFAULT_WORKGROUP_SIZE` /
`RADIX_BITS` from `renderer/gpu_sorting_constants.h`; `CHAINING_FACTOR` from
`renderer/gpu_sorter.h`) are parsed from their defining headers. So a syntax break in
a sorter shader string — or an edit to a helper fragment or a constant — fails this
check instead of matching a stale copy. Radix permutations span the runtime-validated
axes: key_bits `{32,64}`, workgroup `{64,128,256,512}`, radix_bits `{4,8}`, and
subgroups on/off. The coverage self-check (`_validate_sorter_coverage`) is
fail-closed: if a template anchor stops resolving or an axis endpoint is unexercised,
the run exits non-zero rather than silently dropping coverage. Editing the embedded
GLSL in either `.cpp`, or any of the parsed constant headers, re-triggers this
workflow.

## Expected Artifacts

- `summary.json`: machine-readable report with matrix coverage, contract checks, and compile results (including the `sorter_coverage` block and per-permutation `sorter_*` compile results).
- `spv/*.spv`: one compiled SPIR-V file per matrix entry + variant + shader stage, plus one `sorter.<family>.<variant>.compute.spv` per embedded sorter permutation.
