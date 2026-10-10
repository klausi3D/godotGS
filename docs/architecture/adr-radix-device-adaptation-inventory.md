# ADR: Device-aware radix workgroups and additive GPU coverage inventory

- Status: Proposed for maintainer disposition.
- Risk class: R3 because the change updates the renderer release-gate manifest.
- Tracking: #539.

## Context

Increasing the shared sort workgroup default to 512 also increased the fixed
Bitonic and OneSweep requirements. Devices supporting 256–511 invocations then
lost the production indirect 64-bit radix sorter. The factory cannot substitute
Bitonic or OneSweep for that contract.

The renderer correction separates the fixed 256-thread shader size from the
512-thread radix preference. Radix support probing and initialization select
the largest validated size no greater than that preference that fits the actual
X-dimension, invocation and scatter shared-memory limits. Explicit smaller
preferences remain unchanged when supported. A reduction is diagnosed and
existing buffer-size admission uses the effective size.

## Decision

Add a GPU regression case to the already-required `GpuSorting` batch. It checks
the real capability probe, indirect 64-bit factory selection, initialization
and an indirect 64-bit sort against a CPU reference. Test-only ceilings can
lower actual device limits and restore them on scope exit; they cannot raise
a hardware capability. Include an uncapped default and an explicit smaller
preference.

Refresh only the manifest's derived inventory: 173 to 174 cases, 107 to 108
running in named batches, and the exact source/name digest. Deferred coverage
remains 3 cases and the unbatched backlog remains 63. No waiver, exclusion,
minimum, threshold, batch requirement or ratchet is relaxed. The existing
coverage guard also requires the new oracle's batch selection and order
assertion. Removing either must make that guard fail.

Recompute the inventory with `check_renderer_release_gates._requires_gpu_snapshot`
and `python tests/ci/test_gpu_harness_deferred_contract.py --print-summary`,
then run both contract suites and the full module guard suite.

## Chronology and disposition

The renderer implementation initially classified as R2. Adding its regression
case made the existing inventory guards fail, exposing the necessary manifest
update and resulting R3 classification. The source implementation and first
inventory update preceded this permanent ADR. This document records that
sequence; it does not claim the required permanent design record existed
before implementation. The maintainer must disposition that process gap, as
well as the final R3 change and its CODEOWNER review, before merging.

## Consequences and limits

The 512 preference reduces workgroup-prefix work on capable devices; selecting
256 preserves the previous default on smaller devices. Smaller workgroups can
require more histogram memory and more prefix work. Runtime/GPU evidence,
static temporal stability, orbit measurements, allocated GPU memory and two
independent final-diff reviews remain required. NVIDIA measurements do not
prove AMD or Intel behavior.

The inherited single-dimension dispatch limit remains a separate concern. At
a 65,535-group X limit, 256 threads cover 16,776,960 records and 512 threads
cover 33,553,920; explicit 128-thread configurations already have less
headroom. This change does not introduce a count-aware multidimensional
dispatch or alter capacity admission.

Rollback is to revert the radix preference and device-sizing change together
with its new regression registration, then recompute the exact inventory.
