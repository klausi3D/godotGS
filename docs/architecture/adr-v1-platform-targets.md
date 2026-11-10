# ADR: v1.0 platform and graphics-API targets — Vulkan on Windows and Linux, NVIDIA and AMD

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
- **Decision:** D-3 of the 2026-11 cleanup audit. Task CLN-D03. Bundled with
  [Reference parity at default settings](adr-reference-quality-contract.md) (D-4); the two
  decisions are independent.
- **Risk:** this document is R0 (`docs/**`). It changes no code, build option, workflow,
  threshold or gate. The CI lanes it calls for are separate tasks ([Follow-up tasks](#follow-up-tasks)).
- **Refs:** #596, #1160.
- **Base:** `b58bbe35d1f` (origin/master). Every `file:line` below was read at that commit.
  Paths are relative to the repository root.

## Problem

[Release Acceptance Bar §2](../governance/release-acceptance-bar.md#2-supported-platforms)
lists a v1.0 rung per operating system but names no graphics API and no GPU vendor. The module
builds for Windows, Linux and macOS (`SUPPORTED_PLATFORMS = {"linuxbsd", "windows", "macos"}`,
`modules/gaussian_splatting/config.py:1`). Its compute paths have run only on Vulkan, and
nothing records whether D3D12 or Metal belongs in v1.0.

No CI workflow targets a second GPU vendor. The self-hosted GPU jobs target Linux runners
(`runs-on: [self-hosted, Linux, X64, gpu]` and `[..., gpu, rtx3080]`, for example
`.github/workflows/baseline_qa.yml:571` and `.github/workflows/beyond_vram_flythrough.yml:196`).
The one Windows GPU job (`.github/workflows/release_builds.yml:1173`) is skipped unless the
repository variable `GODOTGS_WINDOWS_GPU_RUNNER` is `'true'` (`:1183`). No workflow targets an
AMD, Intel or Apple runner (audit finding B-06).

The code has vendor-sensitive constructs that no CI lane exercises. The default radix scatter
assumes a constant subgroup size of at least 32 and contiguous lane-to-subgroup mapping
(`subgroup_index = lid / gl_SubgroupSize`,
`modules/gaussian_splatting/renderer/gpu_sorter.cpp:1800-1803`; audit finding B-05). Vulkan
does not guarantee either without subgroup-size control, and a mis-ranked scatter produces a
wrong sort with no error.

## Options considered

These are the options in the audit's decision list (D-3).

### Option A — Windows Vulkan only, NVIDIA and AMD validated

- **Cost:** one extra GPU lane, on AMD hardware.
- **Enables:** cross-vendor sort correctness (B-05) is tested; one API keeps one set of compute
  assumptions.
- **Limits:** Linux is not a v1.0 target; no D3D12, Metal, mobile or web.

### Option B — add D3D12

- **Cost:** Godot supports D3D12, but the module's compute paths and subgroup assumptions are
  untested there; a D3D12 GPU lane and a pass over every compute shader.
- **Enables:** Windows users on the D3D12 driver.
- **Limits:** a second API adds a second compute-correctness surface before the first is
  validated on two vendors.

### Option C — add Metal and macOS

- **Cost:** macOS is only `build-supported` today, and Apple GPUs need the sort's
  forward-progress assumptions removed; an Apple GPU lane.
- **Enables:** macOS as a target platform.
- **Limits:** v1.0 waits on the sort rework.

### Recommendation

The audit recommended Option A for v1.0, stated in the bar: an AMD card in CI costs less than
any of the sort-correctness risks it would expose. Only the first game should drive B and C.

## Owner decision

**Vulkan on Windows and Linux, validated on NVIDIA and AMD**, decided by the owner on
2026-11-10. D3D12, Metal, mobile, web and XR are out of v1.0.

This differs from the recommendation: it is Option A widened to Linux. Options B and C are
rejected for v1.0 for the reasons in [Options considered](#options-considered). Adding Linux costs a Linux AMD lane next to the
Windows one, and the bar's existing Linux requirements ([Consequences](#consequences)) stay.

The owner's approving review on the pull request that adds this record is the acceptance
record; there is no separate issue comment.

## Decision

| Platform | Graphics API | GPU vendors validated for v1.0 | In v1.0 |
| --- | --- | --- | --- |
| Windows | Vulkan | NVIDIA, AMD | yes |
| Linux | Vulkan | NVIDIA, AMD | yes |
| macOS | — | — | no; the module still builds (`build-supported`) |
| D3D12 (Windows) | — | — | no |
| Mobile, web | — | — | no |
| XR / multiview | — | — | no |

The rungs in the release bar (Windows `production-tested`, Linux `editor-tested`) are
unchanged. The owner did not change them; see [Open questions for the owner](#open-questions-for-the-owner).

## Consequences

### For the codebase and CI

- **An AMD GPU lane is a v1.0 requirement. It does not exist yet.** CLN-EV12-2 creates it,
  inert and visibly skipped until an AMD runner is registered. It needs AMD runner hardware,
  which the owner has to provide.
- No code changes in this record. `SUPPORTED_PLATFORMS` keeps `macos`; whether the macOS build
  stays supported after v1.0 scoping is open ([Open questions for the owner](#open-questions-for-the-owner)).
- The Linux requirements already in the bar stay: Linux at `editor-tested` needs the `qa` and
  `sorting` categories on a GPU-backed Linux lane. CLN-SH05 proves and pins those lanes.

### For the cleanup plan

| Item | Effect |
| --- | --- |
| B-05 → CLN-SO02 | Unblocked: remove the radix scatter's dependence on subgroup size. |
| B-06 → CLN-EV12, CLN-EV12-1, CLN-EV12-2 | Unblocked and committed: forced-variation batches and the AMD lane. |
| CLN-SH05 (#596) | Unblocked: prove and pin the Linux GPU lanes. |
| CLN-ST05 | Unblocked: GPU-tier presets and runtime tier detection, for the two vendors above. |
| CLN-LT09, CLN-LT09-1 to CLN-LT09-4 (#1160) | Void: XR is out of v1.0. This record and bar §2 carry the XR exclusion that CLN-LT09-1 offered as its alternative. |
| CLN-RT05 | Its dependency on CLN-LT09 no longer holds. |
| D3D12 and Metal rows | Out of v1.0. The compatibility matrix gains no row for them. |
| Runner purchase | An AMD GPU runner. No D3D12-only or Apple runner is needed for v1.0. |

### What stays the same

- The [Compatibility Matrix](../reference/compatibility-matrix.md) records current evidence,
  not targets. It is generated from `docs/reference/compatibility_sources.yaml` and is not
  edited here; no row is promoted.
- No threshold, gate or workflow changes.

## Follow-up tasks

- CLN-EV12-1 and CLN-EV12-2: forced-variation batches and the AMD lane.
- CLN-SO02: subgroup-size-independent radix scatter.
- CLN-SH05: Linux GPU lanes for `qa` and `sorting`.
- to-file: register an AMD GPU runner (hardware; owner).

## Out of scope

- Any code, build or workflow change; each task in [Follow-up tasks](#follow-up-tasks) cites this record.
- Which AMD and NVIDIA models count as validated, and driver versions.
- Performance targets per vendor.
- Platforms after v1.0: D3D12, Metal, mobile, web and XR come back only through a new ADR.

## Open questions for the owner

1. The Linux v1.0 rung: the bar keeps `editor-tested`. Does "validated on NVIDIA and AMD"
   raise it to `production-tested` like Windows?
2. macOS: keep `macos` in `SUPPORTED_PLATFORMS` as an unvalidated build, or drop it.
3. When the AMD runner is expected, so that CLN-EV12-2 can move from skipped to required.
