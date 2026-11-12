# GitHub Actions Workflows

This directory contains 10 active workflow files.

GitHub's Actions tab can also show historical workflow names from past runs, disabled files, or workflow files that are no longer present in this directory. This README tracks the workflow files currently checked into `.github/workflows/`.

## CI layout (CI diet, maintainer approval 2026-11-07)

The maintainer approved this layout on 2026-11-07. Goal: a PR's CI finishes in
about 15-20 minutes and a merge costs about 30 instead of about 110 self-hosted
runner-minutes, without deleting a check that catches product bugs. Every moved job
names where its coverage runs now (PR body of the CI-diet PR, and the table in
`docs/reference/build-test-ci.md`).

| When | What runs | Self-hosted runner |
| --- | --- | --- |
| Every PR / merge-queue entry | `agentic-pr-gate` (required; the **only** run of the guard phase, `run_module_tests.py --guard-only --guard-self-tests auto`), `docs-build` (required), `baseline_qa.yml` (`changes`, hosted `cpu-tests`, and one self-hosted `gpu-tests` job that builds once from the persistent scons cache), `gaussian_shader_validation.yml` (path-filtered) | `gpu-tests` only, and only when the PR touches code or CI inputs; fork PRs never |
| Push to `master` | `docs_pages.yml` deploy only | none |
| Nightly on `master` HEAD | `gaussian_shader_validation.yml` 00:15, `baseline_qa.yml` 00:30 (every path filter off, full guard phase incl. every guard self-test, all doctest lanes, all GPU lanes, evidence collector, harness baseline update), `release_builds.yml` 02:30 (Linux + Windows, export templates, finite-math guard, export smoke, publish, prune), `release_ci_runtime.yml` 06:00 | staggered so no two nightlies share the runner |
| Weekly | `gaussian_production_gates.yml` open-world proof (Mon 03:30), `codeql.yml` (Sun 04:00, hosted), `beyond_vram_flythrough.yml` fly-through lane (Sat 22:00, report-only) and its liveness watchdog (Mon 06:00, hosted) | open-world proof; beyond-VRAM `flythrough` (RTX 3080) |

**Accepted risk (maintainer approval 2026-11-07):** per-merge release builds and
duplicate gate runs moved to nightly; up to one-day detection window for regressions
on master accepted. A regression that only appears when two individually green PRs
combine, a release-packaging or export-smoke regression, and a guard self-test that
a PR's path gate skipped are caught by the next nightly instead of on the merge. The
same record is in `.github/workflows/AGENTS.md` and
`docs/governance/review-policy.md`.

## Active Workflows

| Workflow | File | Purpose | Notes |
| --- | --- | --- | --- |
| Baseline QA Automation | `baseline_qa.yml` | The per-PR CI and the nightly full run: module doctest lanes, PLY/pipeline/runtime categories and the headless-ci profile (hosted `cpu-tests`); GPU sorting / render-thread characterization, the blocking QA-scene visual gate, the golden-image GPU harness, the blocking `streaming-gpu-ci` runtime gate and the production evidence collector with readiness enforcement (self-hosted `gpu-tests`). | Triggers: `pull_request`, `merge_group`, `schedule` (00:30 UTC), `workflow_dispatch`; no push. `changes` (hosted, API only) decides per PR: no code change runs neither `cpu-tests` nor `gpu-tests`; the harness, the evidence collector and the two slow streaming scenarios keep their own path filters; the nightly turns every filter off. `gpu-tests` builds in-job from `/opt/actions-runner/godotgs-scons-cache` (no artifact round trip). `cpu-tests` runs `run_module_tests.py --skip-guards` on PRs (the guard phase ran in `agentic-pr-gate`; the binary-dependent guards still run) and the full phase nightly. The `qa` scene category runs on the real display with `--qa-require-capture --require-qa-baseline` (#522) — headless cannot create a RenderingDevice, so the scenes would capture nothing and the category would degrade to a skip. That step is compare-only and never rewrites its own baseline. |
| Docs Pages (Versioned) | `docs_pages.yml` | Checks the docs on every PR (`docs-build`) and builds and deploys MkDocs docs with mike versioning to `gh-pages` (`deploy`). | `docs-build` runs on every `pull_request` (no path filter), `merge_group`, push and dispatch on `ubuntu-latest` with `contents: read`, never deploys, and has no job-level `if:`. It regenerates the docs artifacts, fails if a committed generator output is stale (`git diff --exit-code`), stages, checks the media budget and runs `mkdocs build --strict`; `scripts/docs/release_acceptance.py` runs report-only (`continue-on-error`). `docs-build` **is** a required status check on `master` (see [Required Checks](#required-checks)). `deploy` (`contents: write`) runs only on push/dispatch: it publishes `latest` from `master/main` and versioned docs from `v*` tags, and does not depend on `docs-build`. Only `latest` publishes the Doxygen C++ API; for a `v*` tag, `scripts/docs/redirect_cpp_api_to_latest.py` replaces the staged `api/cpp` tree with a redirect page to latest's copy before the build. |
| Gaussian Production Gates | `gaussian_production_gates.yml` | Weekly non-blocking open-world proof evidence (`openworld-proof-dev`, `openworld-proof-weekly`). | `schedule` (Mon 03:30) and `workflow_dispatch` only since the CI diet: its former per-PR jobs moved (`guards` -> `agentic-pr-gate` + nightly; `module-validation` -> `baseline_qa.yml` `cpu-tests` / `gpu-tests`). Compiles the Linux test editor in `build-linux-editor` (self-hosted, no `gpu` label) and hands it to `openworld-proof-evidence` as a commit-pinned artifact (see [Build and GPU-test split](#build-and-gpu-test-split)). |
| Gaussian Shader Validation | `gaussian_shader_validation.yml` | Validates shader compile matrix and host/shader contract checks. | Focused shader CI gate. Path-filtered `pull_request` and `merge_group`, nightly `schedule` (00:15) without a filter, `workflow_dispatch`; no push. |
| CodeQL | `codeql.yml` | CodeQL code scanning (`c-cpp`, `python`, `actions`, `build-mode: none`). | GitHub-hosted, weekly (Sun 04:00) + `workflow_dispatch`; `security-events: write` on the job only. Replaces GitHub's CodeQL default setup, which must be switched off in the repository settings (advanced-setup uploads are refused while it is on). |
| Release Builds | `release_builds.yml` | Builds Linux and Windows editors for CI artifacts, nightly prereleases, and stable-tag publishes, plus the Linux and Windows `target=template_release` export templates. | Triggers since the CI diet: `schedule` (02:30), `push` of `v*` tags, `workflow_dispatch`; no `pull_request` and no branch push, so nothing is path-filtered and every lane runs on master HEAD each night. Publishes Linux tarballs and Windows zips on the nightly schedule and on `v*` tag pushes. Whenever a Windows editor zip ships, the Windows export template zip ships with it: `release_candidate_gate` attests it and `publish_release` re-verifies it with `--require` (#994). The Windows editor is optimized (no `dev_build`) on every channel. The Linux nightly editor stays `dev_build=yes`. The `finite_math_guard` job blocks publication on every channel, and the `release_candidate_gate` job gates the stable/tag publish path (both-platform builds + `--mode candidate` validation, fail-closed); see below. The two export-template jobs (#825) are **not symmetric**, and the difference matters when diagnosing a blocked release. `build_linux_export_template` is **transitively gating**: `export_smoke_linux` lists it under `needs:`, and both `release_candidate_gate` and `publish_release` list *that* job and assert its result. So a failed Linux template build (or Linux editor build) skips the smoke test, and a skipped smoke test blocks **every stable/tag publish and every nightly**. Nothing in either publication `if:` names the template job directly; the block runs entirely through `export_smoke_linux`, which is why a red Linux template build presents as a *skipped* smoke test rather than as a failed one. `build_windows_export_template` (GitHub-hosted `windows-latest`) is **inside the publication closure**: `release_candidate_gate` and `publish_release` list it under `needs:` and require its success whenever `build_windows` succeeded, because they download, attest and `--require` the Windows template zip (#994); without that edge the gate could run before the template build finished and die with "artifact not found", blocking Linux too. It is built and attested but not executed. The Linux template that `export_smoke_linux` executes is not itself a published asset (only the Windows template zip is attached to releases). Kept honest by `tests/ci/test_release_publication_gating.py`, which derives the transitive `needs:` closure of the release-side-effect jobs and fails if this README or `release_builds.yml` still describes a job inside that closure as ungated. See [export templates](../../docs/development/export-templates.md). The blocking export smoke is `export_smoke_linux`: it runs `tests/runtime/run_export_smoke.py --target linux` against the Linux template built by the same run on the self-hosted Linux GPU runner (RTX 3080, real Xorg at `DISPLAY=:0`) — it exports the test project and launches the exported binary, plus a negative control that requires an empty `custom_template/release` to be rejected for the missing-template reason specifically (a timeout, a crash or an unrelated error fails the control). It is wired **into the publication dependency graph**, not beside it: `release_candidate_gate` and `publish_release` both list it under `needs:` (which is what makes publication wait for it) and both assert `result == 'success'` (which is what makes a failure block, since under `always()` a `needs:` entry alone gates nothing). It is required on every publishing channel with no outage tolerance: the Linux payload ships on every channel. **The Windows exe is no longer executed by any gating lane.** `export_smoke_windows` (the same script, `--target windows`) is kept, routed to `[self-hosted, Windows, X64, gpu, d3d12]`, and is **skipped unless the repository variable `GODOTGS_WINDOWS_GPU_RUNNER` is `true`** (there is no Windows GPU runner today; without the variable a job targeting those labels would queue for about 24 h and, with `cancel-in-progress: false`, hold every later release run behind it). It is deliberately not wired into `release_candidate_gate` or `publish_release`: it is in no `needs:` list and no `if:`, so it blocks nothing whether it runs or is skipped, and the Windows editor and template zips ship built (on `windows-latest`) but unexecuted. Kept honest by `tests/ci/test_release_publication_gating.py`, which evaluates both `if:` conditions over a truth table. |
| Agentic PR Gate | `agentic_pr_gate.yml` | Fork-safe, always-on gate: validates every workflow plus the agentic control plane, runs the validator/agentic tests, the agentic/governance link check, and the GPU-free `--guard-only` lane. | GitHub-hosted (`ubuntu-latest`); installs its PyYAML parser from the version-and-hash-pinned `tests/ci/requirements-automation.txt`, runs on every PR and the merge queue. Required status check (job name): `agentic-pr-gate`. |
| GS-COMBINED-1 Gate | `gs_combined_1.yml` | The game-scene acceptance gate of `docs/architecture/adr-one-route-many-producers.md` section 9: a streamed world (variant A chunk-streamed, variant B V4 paged HLOD), 32 movable splat objects, a pick target, an animated mesh character and five shadowed lights in one frame, judged per assertion by `tests/runtime/gs_combined_1_verdict.py`. | Two jobs. `gs-combined-1-gpu` builds an optimized editor with tests and runs `run_runtime_validation.py --profile gs-combined-1` on the self-hosted Linux GPU runner (RTX 3080), with the per-job fork guard; it uploads the records, verdicts and screenshots, and writes the per-assertion table to the job summary. `gs-combined-1` is the status: GitHub-hosted, `needs:` the GPU job, `if: ${{ always() }}` as its only job-level condition, and passes only when the GPU job's result is `success`. Runs on `pull_request`, the schedule and dispatch (no push trigger: nothing self-hosted runs on a push to `master`). **Reported, not required**: it is red until every assertion is live; `tests/runtime/gs_combined_1_expectations.json` lists each blocked assertion with its issue, and a blocked assertion is never green. Making `gs-combined-1` required is a maintainer decision, taken once that file blocks nothing. On `pull_request` the workflow is **path-filtered** (renderer, shaders, lod, core, io, nodes, the gate's own `tests/runtime/gs_combined_1*` and `test_gs_combined_1*` files, `run_runtime_validation.py` and the workflow file); the schedule and dispatch are unfiltered. A workflow-level path filter skips the whole workflow, the status job included, so a filtered PR reports **no** `gs-combined-1` status. That is acceptable only while the check is not required: **before `gs-combined-1` is made required, the path filter must be replaced with in-job change detection** (a job that always runs and reports), or every PR outside those paths blocks on a check that never reports. |
| Release-CI Runtime Evidence | `release_ci_runtime.yml` | Nightly + manual evidence lane for the canonical release-ready runtime profile `release-ci` (non-headless GDScript runtime suite + required renderer proof). | Self-hosted Linux GPU runner (RTX 3080). **Not a required PR gate** — schedule + `workflow_dispatch` only. Runs `run_runtime_validation.py --profile release-ci --gd-mode gpu-vulkan --skip-cpp` (`gpu-vulkan` resolves to the `x11` display driver on Linux, against the runner's real Xorg at `DISPLAY=:0`). |
| Beyond-VRAM Fly-through | `beyond_vram_flythrough.yml` | BV-06b of `docs/architecture/adr-beyond-vram-worlds.md` (sections 9.1, 9.2, D-14): the full beyond-VRAM fly-through lane, grown from BV-06a's minimal dispatch probe. | `schedule` weekly Saturday 22:00 UTC (the lane, Tier A, at night so the single GPU runner is free during the day) and Monday 06:00 UTC (the liveness watchdog), plus `workflow_dispatch`; no PR or push trigger; **report-only, not a required check, gates nothing**. `build-linux-editor` (self-hosted, no `gpu` label) compiles the optimized producer binary (`target=editor optimize=speed_trace dev_build=no tests=yes`, ADR D-10) into its own object cache `/opt/actions-runner/godotgs-scons-cache-opt` (`cache_limit=15`) and hands it over as a commit-pinned artifact. `prime-world-cache` (dispatch with `prime_world_cache`, pinned to `[gpu, rtx3080]` because the cache lives on that disk, 240 min) runs `tests/runtime/flythrough/dispatch_lane.py prime-cache` under the cache's exclusive `flock`: sweep orphan keys, clear `quarantine/`, delete the old key delete-first, refuse `DID NOT RUN: disk` below the world budget + the 25 GiB margin, generate (BV-05), `fdatasync` + `POSIX_FADV_DONTNEED` every world file (D-14 item 1), write `.complete` last. `flythrough` (pinned to `[gpu, rtx3080]`, 150 min) runs the GPU preflights, then `dispatch_lane.py`: `df` preflight (disk probe + margin; the cached worlds already hold their bytes), O_DIRECT `dd` read/re-read with the D-14 `host_cache` label, `vulkaninfo` limits, `build_info`, `cache-check` (`NOT_RUN: cache absent` / `cache_stale` when the generator sources changed since the prime; every digest verified against the world's manifest, a mismatch moves the key to `quarantine/` and is INVALID; then fadvise again), and the BV-07 probe on the cached worlds, which first requires `mincore` residency <= 1 % (else INVALID `cache_warm`, probe not started) and records the probe tree's block-read bytes with the path oracle's expected unique stored bytes of every probed world: the D-14 record the gate verdict's V10 entry judges (BV-13a-2b; residency <= 1 % and read bytes >= 0.9 x the oracle, else INVALID `cache_warm`; no measurement is MISSING, never PASS). Right after `build_info`, `dispatch_lane.py doctest-proofs` (BV-13a-2b) runs the ADR 8.5 doctest proofs on the same release-tests binary, each case by exact name (C-thrash today; G7/G8/G9/G3/G2a/G10h-a are listed absent until their counters exist), in every run mode; no binary is NOT_RUN. Then `dispatch_lane.py controls` (BV-13a) runs the ADR 8.5 forcing controls on the cached T0 (`tests/runtime/flythrough/flythrough_controls.py`: C-starve, C-off, C-settings, C-warm), each asserting its exact `(gate, verdict, reason)` tuple through the BV-09 verdict module and judging V10 on its own D-14 record; a differing tuple or a harness failure makes the lane INVALID. They run after the cold probe because a control warms T0's pages. The contention postflight's outcome and every step record become one lane verdict (VOID > FAIL > INVALID > NOT_RUN > PASS, missing record = NOT_RUN) with the section 9.1 exit code; uploads `beyond-vram-flythrough-<tier>-<run_id>-<attempt>` (without the screenshots), `beyond-vram-screenshots-<tier>-<run_id>-<attempt>` (the probe's viewport PNG at every checkpoint, main run and controls) and `bvw-verdict-<run_id>-<attempt>` (`verdict.json`). Cache layout: `/opt/actions-runner/godotgs-world-cache/{CACHE_FORMAT,.lock,<world_id>/<gen_sha12>/,quarantine/}`. `codec-probe` (self-hosted, no `gpu` label, BV-C2, dispatch only) runs the same binary's GSQ codec probe (`tests/runtime/flythrough/run_codec_probe.py`) and uploads `gsq-codec-probe-<run_id>-<attempt>`; report-only. `liveness-watchdog` (GitHub-hosted, `actions: read` on this job only; Monday schedule or dispatch with `mode=watchdog`) runs `tests/runtime/flythrough/lane_watchdog.py`: before M2 it fails unless a scheduled run in the last 9 days ended PASS, FAIL or INVALID (VOID, NOT_RUN and a missing verdict do not count) and on any run queued > 20 h; mode `m2` (BV-14) adds PASS within 9 days, two consecutive VOIDs and cancelled or replaced runs. Dispatch `mode=auto_tree` runs only the build and `prime-auto-tree` (RTX 3080, 240 min): scratch T0 plain-GSQ to V4 build, measured Tier A RAM/disk admission, two Tier A builds with repeat identity and oracle evidence, then verification of both cache keys. It requires a fresh `A-plain_v3_gsq` key and never re-primes it. Only JSON/log reports are uploaded. Concurrency group `bvw-<event>-lane` / `bvw-<event>-auto-tree` / `bvw-<event>-watchdog`, never cancelled, so the watchdog can never replace the queued run it reports on. |

## Required Checks

`agentic-pr-gate` (the job name in `agentic_pr_gate.yml`, shown in the PR checks UI
as `Agentic PR Gate / agentic-pr-gate`) and `docs-build` (the job name in
`docs_pages.yml`, shown as `Docs Pages (Versioned) / docs-build`) are the two
required status checks on `master`. Live protection, read back with
`gh api repos/klausi3D/godotGS/branches/master/protection` on 2026-10-08:
`contexts: ["agentic-pr-gate", "docs-build"]`, `strict: false`, `enforce_admins: true`,
`required_conversation_resolution: true`, `required_approving_review_count: 0`, force
pushes and branch deletion blocked, no rulesets. See
`docs/governance/github-settings.md` for the full table. An earlier revision of this
section described the check as merely *intended* for branch protection; it was
already live (`GS-AUDIT-DOC-003`). `docs-build` is the second required context
(first observed 2026-10-08) and was built for it: no path filter on `pull_request` and no
job-level `if:`, so it always reports a terminal status.

Because `enforce_admins` is `true` and both contexts are required, a broken edit to
`agentic_pr_gate.yml` blocks every merge in the repository, including its own fix,
and a red `docs-build` blocks a merge exactly as a red `agentic-pr-gate` does, admins
included. Every other lane — GPU, runtime, visual, release — is advisory at the merge
boundary.

It runs only on GitHub-hosted runners, so external fork PRs always receive a status
without touching the self-hosted lanes. It runs:

- `python tests/ci/test_validate_automation.py -v` followed by
  `python tests/ci/validate_automation.py --contracts-only` after installing the
  version-and-hash-pinned PyYAML dependency; the validator derives and parses
  every `*.yml` and `*.yaml` workflow, and fails closed if the parser or corpus
  is absent, or if a document lacks a typed, non-empty top-level trigger or
  `jobs` mapping
- `python scripts/agentic/validate_repo_contract.py --strict-hierarchy` (the
  `--strict-hierarchy` flag also requires the AGENTS.md hierarchy and
  `docs/governance/*`; without it those could all be deleted with the gate green)
- the `scripts/agentic` contract validators as a **self-test against the shipped
  templates** — fixtures, not this PR (see the limit below)
- `python scripts/agentic/classify_change.py --base-ref <PR base> --format json
  --github-step-summary` — derives the risk class from the PR's own diff on both
  `pull_request` and `merge_group`, and fails the check when the base cannot be
  resolved
- `python -m unittest discover -s tests/agentic`
- `python scripts/docs/check_links.py docs README.md BUILDING.md CONTRIBUTING.md AGENTS.md CLAUDE.md`
- `python tests/ci/check_doc_snippets.py` — GDScript examples, `Class.member` prose
  references and API-page method tables in `docs/` against the module's ClassDB
  bindings; no path filter, so a binding rename fails the PR that makes it
  (`docs/architecture/` and ADRs only warn)
- `python tests/ci/run_module_tests.py --guard-only --guard-self-tests auto` (GPU-free;
  the StringName guard self-skips when no Godot binary is present). This is the only
  per-PR run of the guard phase. `auto` skips the path-gated guard self-tests
  (`PATH_GATED_GUARD_SELF_TESTS` in `run_module_tests.py`) when the PR's diff touches
  none of `GUARD_SELF_TEST_TRIGGER_PREFIXES` (or a root-level file); every check
  against the tree still runs, every skip is printed, an unresolvable base runs them
  all, and the nightly runs all of them

The link check covers the full docs tree plus the root governance docs (only paths
present in the tree are passed, so it is robust on partial trees and is the full-docs
check on `master`).

**Limit of what this gate enforces.** The risk class is enforced *from the diff*: an
unresolvable base fails the check, and an empty changed-path set falls back to
`classification.default_unclassified` (R3) rather than R0. The class's
`evidence_requirements` and `deterministic_checks` are **published** to the job
summary for the merging human, not verified. Per-PR **scope** (`owned_paths` /
`forbidden_paths`) and evidence contracts are **not consumed by CI at all**, because
the repository has no per-PR contract source — `check_pr_contract.py` only ever sees
`.agentic/templates/task.json`. Wiring a contract source is the Phase-2
contract-source ADR.

## Manual Dispatch Inputs

| Workflow | Input | Options |
| --- | --- | --- |
| `baseline_qa.yml` | `debug_mode` | `true`, `false` |
| `baseline_qa.yml` | `baseline_mode` | `compare`, `update` |
| `beyond_vram_flythrough.yml` | `mode` | `flythrough` (default, the lane), `auto_tree` (build and prime `A-v4_gsq_auto` only), or `watchdog` (only the liveness watchdog) |
| `beyond_vram_flythrough.yml` | `tier` | `T0` (default, 4.19M-splat smoke world), `A` (the 201M-splat world, uncompressed plain v3; also uses `T0`). The weekly schedule runs `A`. |
| `beyond_vram_flythrough.yml` | `prime_world_cache` | boolean (default `false`): regenerate the tier's worlds into the runner's world cache first (job `prime-world-cache`) |
| `beyond_vram_flythrough.yml` | `disk_probe_gib` | integer string `1`-`64` (default `8`), size of the O_DIRECT disk-probe file |
| `beyond_vram_flythrough.yml` | `scan_dir` | runner directory of real scans for `codec-probe` or optional `auto_tree` identity evidence (default empty: `$GS_REAL_SCAN_DIR`). Codec absence is `MISSING`; auto-tree real-scan identity is `NOT_RUN` unless a scan with at most 1M leaves and a bound tiled/in-core digest API are available. |
| `beyond_vram_flythrough.yml` | `watchdog_include_dispatch` | boolean (default `false`), `mode=watchdog` only: also count `workflow_dispatch` runs, not only scheduled ones |
| `beyond_vram_flythrough.yml` | `controls_only` | boolean (default `false`): run only the BV-13a forcing controls on T0, not the main fly-through probe |
| `baseline_qa.yml` | `run_gpu_lane` | `true`, `false` (run the production evidence collector on a dispatch) |
| `baseline_qa.yml` | `enforce_gpu_readiness` | `true` (default), `false` (manual diagnostic run only; pull_request, merge_group and schedule always enforce, #1164). Enforced: `897_907_ready`, `900_ready`, `815_ready`; `943_ready`, `871_ready`, `902_ready` are reported, not enforced, until #1241 (maintainer decision 2026-10-13; the report-only set may only shrink) |
| `baseline_qa.yml` | `runtime_loops` | integer string |
| `gaussian_production_gates.yml` | `run_openworld_proof_dev` | `true`, `false` |
| `gaussian_production_gates.yml` | `run_openworld_proof_weekly` | `true`, `false` |
| `release_builds.yml` | `publish_channel` | `none`, `nightly`, `stable` |
| `release_builds.yml` | `release_tag` | string (`vX.Y.Z` when `publish_channel=stable`) |
| `release_builds.yml` | `release_name` | optional string |
| `release_builds.yml` | `keep_nightlies` | integer string |

`mode=auto_tree` requires the existing `A-plain_v3_gsq` key to be complete and
fresh. It ignores `tier`, `encoding`, `prime_world_cache` and `controls_only`;
it never runs the fly-through, codec probe or watchdog. T0 runs first, stays in
scratch and preserves the existing T0 cache, bounded by its own smaller job
limits. The following Tier A admission records and prints `MemAvailable`,
`MemTotal`, `SwapTotal`, `SwapFree` and `df` capacity; missing memory/swap fields
are null with an explicit absence reason. Admission refuses below 20 GiB
available RAM or `2 × 10.2 GB + 12 GiB` free disk. Missing required memory
information is `NOT_RUN`; swap is diagnostic and does not increase the RAM budget.
A refused preflight makes the job fail and skips Tier A prime and cache-check,
with Tier A `NOT_RUN` reports explaining the refusal. T0 evidence is preserved,
and report upload runs with `if: always()` even after a refusal. Tier A writes
`A-v4_gsq_auto/<gen_sha12>-<tree_sha12>`, checks two builds' file SHA-256 and
`content_generation`, verifies V4 records and runs the eight-pose oracle with its
leaves-only control. The budget remains 1,000,000 with `budget_frozen=false`.
The 240-minute job allows a 30-minute T0 step and a 200-minute Tier A step for
two 90-minute build deadlines plus verification. The artifact includes only
`bvw-auto-tree-out/**/*.json` and `**/*.log`, including contention reports;
world files remain on the runner. A stale or absent plain key requires a
separate plain-cache prime; this mode never silently rebuilds it.
Tier A uses 16 workers (within the design's 12–24 range), with RAM admission
capped at 20 GiB; scratch and output limits are 12 GiB each. T0 uses four workers,
16 GiB RAM, 2 GiB scratch/output limits and a 20-minute build deadline. Producer
scratch stays on the output filesystem. T0 does not require Tier A's capacity
admission. The T0 RAM limit covers the current tiled reservation (10.431 GiB
before variable source/sidecar overhead) with margin. Tier A limits remain
unchanged: preflight/prime records include a diagnostic formula estimate,
not a new gate. Current permanent tiled admission alone is 362.456 GiB;
a full 16-worker batch subtotal is 393.451 GiB before variable overhead.
The later retained SH3 payload alone is 64.406–72.721 GiB for the design's
36M–67M interiors. These are reservation calculations, not measured RSS;
the builder's own refusal remains authoritative. Failure reports preserve
`builder_raw` (including reservation, page/tile counters and reason text),
`producer_reason`, and builder wall time when supplied.
Reports include both build identities, wall time, measured RSS,
per-level eps, W13 node counts, V4 verification, oracle cuts and the leaves-only
control. Unmeasured scratch peaks carry an explicit absence reason. Real-scan
identity is `NOT_RUN` because the existing bound in-core API exposes no per-node
digests; no synthetic fixture substitutes for that evidence.

## Renderer Release Gate Contract

The renderer/public-alpha evidence policy is maintained in
`docs/reference/renderer_release_gate_manifest.json` and validated with:

```bash
python tests/ci/check_renderer_release_gates.py --mode contract
```

The same contract check is part of `tests/ci/run_module_tests.py --guard-only`,
which is what the required `agentic-pr-gate` runs on every PR (and `baseline_qa.yml`
`cpu-tests` runs nightly). The contract check
is deterministic and GPU-free. Public-alpha candidate mode
requires the evidence bundle, a public-alpha channel/tag selector, and a
separate operator-supplied issue-label snapshot through `--issues-json` so P0,
P1, and release-blocker rows cannot be replaced by a snapshot embedded in the
bundle. The gate proves separate filesystem identity only; it does not
machine-enforce the snapshot's GitHub provenance, completeness, capture time, or
freshness. Live acquisition and those stronger guarantees remain owned by
issue #360. The workflow-policy portion of the checker validates required
workflow files and job markers only;
the stronger no-downgrade workflow rules remain documented review policy until
the checker grows a real GitHub Actions behavior parser.

External checks are not automatically renderer release blockers. The only
required contexts are `agentic-pr-gate` and `docs-build` (live state observed
2026-10-08, see [Required Checks](#required-checks) above). The external
status-check policy lives in `external_status_check_policy` in
`docs/reference/renderer_release_gate_manifest.json` and is explained in
`docs/reference/renderer-release-gates.md`; update the manifest before treating
any other external result as part of public-alpha signoff.

### Fast-math finiteness guard (`finite_math_guard`)

`release_builds.yml` carries a `finite_math_guard` job (issue #590). The shipping
configuration (`target=template_release`, which resolves `optimize=auto ->
speed`) compiles the module with GCC/Clang `-ffast-math`, under which the
compiler is free to fold away NaN/Inf checks. No other lane builds
`optimize=speed`, so a regression that re-disables the import and GPU-payload
finiteness guards would otherwise ship silently.

The job builds the module with `optimize=speed` on GCC and runs the
`GaussianData` finiteness doctest against that binary, asserting both that the
doctest passed **and** that the filter actually matched a case (doctest exits 0
on zero matches).

`publish_release` lists the job under `needs:` **and** asserts
`needs.finite_math_guard.result == 'success'` in its `if:`. The explicit result
assertion is required: `publish_release` uses `always()`, so a `needs:` entry
alone would not block anything and a failing guard could sit next to a published
release. The gate applies to every publishing channel, nightly included, because
every channel ships the same module code.

`release_candidate_gate` (below) also lists `finite_math_guard` under `needs:`
and asserts `needs.finite_math_guard.result == 'success'` in its own `if:`. That
puts the finiteness guard *inside* the publication dependency graph rather than
beside it: a failed guard skips the candidate gate, which in turn fails
`publish_release`'s `needs.release_candidate_gate.result == 'success'` check.
`release_candidate_gate` runs under `always()` too, so the same rule applies —
the `needs:` entry alone would block nothing without the explicit assertion.

### Stable-tag publish gate (`release_candidate_gate`)

A `v*` tag push (and a manual stable dispatch) resolves to `channel=stable,
publish=true` in `release_builds.yml`. The `release_candidate_gate` job gates
that publish path (issue #593):

- it fails the stable/candidate path unless **both** `build_linux` and
  `build_windows` succeeded (no Linux-only stable release);
- it runs `check_renderer_release_gates.py --mode candidate` against a
  public-alpha evidence bundle and a separate issue snapshot, and **fails closed**
  when either file is absent, so a tag cannot publish without passing candidate
  validation;
- `publish_release` hard-depends on the gate and sets
  `fail_on_unmatched_files: true` for the stable channel;
- it binds the evidence to reality: `--expected-commit ${{ github.sha }}` (the
  bundle must be for the commit being published) and `--artifact-sha
  <group>=<sha256>` for each archive built in this run (the bundle must record
  the digests of the bytes actually being shipped);
- it writes a `release-attestation.json` over the payload it validated, and
  `publish_release` re-hashes what it downloaded and refuses to publish unless
  every file and the commit match (`tests/ci/release_attestation.py`). A stale
  bundle, a rebuilt artifact, an injected file, or a missing attestation all
  fail the publish closed.

Nightly prereleases are a pass-through (the gate does not run candidate
validation for them) and keep the Linux-only / relaxed-unmatched behavior so a
Windows runner outage cannot stall the nightly cadence.

**Scoped gap:** no CI lane yet produces the candidate evidence bundle (issue
#360), so the gate currently fails closed on every real `v*` tag. A maintainer
cutting a candidate must place both files in the runner workspace and point the
`RELEASE_CANDIDATE_EVIDENCE` and `RELEASE_CANDIDATE_ISSUES` repo/environment
variables at different files. See `docs/reference/renderer-release-gates.md` for
details.

## Runner Trust Boundary (fork PRs)

The self-hosted runners are persistent and must never execute
untrusted code from a **fork pull request**. The self-hosted jobs carry a fork guard
so fork PRs are skipped on those runners, while same-repo (maintainer) PRs, `push`,
and `workflow_dispatch` still run:

```yaml
if: ${{ github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository }}
```

- `baseline_qa.yml` — `gpu-tests` (form above, ANDed with "`changes` found a code change"). `changes` and `cpu-tests` are GitHub-hosted and run for fork PRs too.
- `gaussian_production_gates.yml` — `build-linux-editor` (form above, ANDed with "schedule, or a dispatch asking for an openworld surface"), and `openworld-proof-evidence`, which carries a *narrower* guard: `if: github.event_name == 'schedule' || (github.event_name == 'workflow_dispatch' && ...)`, so it never runs on a pull request at all, fork or same-repo. The workflow has no `pull_request` trigger since the CI diet; the guard is kept fail-closed.
- `beyond_vram_flythrough.yml` — `build-linux-editor`, `prime-world-cache`, `prime-auto-tree`, `flythrough`, `codec-probe` (form above). `schedule` and `workflow_dispatch` only, so the guard is trivially satisfied; it is carried explicitly to keep the self-hosted jobs fail-closed if a `pull_request` trigger is ever added. `liveness-watchdog` is GitHub-hosted.
- `gaussian_shader_validation.yml` — `shader-validation` (form above).
- `release_ci_runtime.yml` — `build-linux-editor`, `runtime-release-ci` (form above). This workflow has no `pull_request` trigger (schedule + `workflow_dispatch` only), so the guard is trivially satisfied; it is carried explicitly to keep the self-hosted job fail-closed if a `pull_request` trigger is ever added.
- `gs_combined_1.yml` — `gs-combined-1-gpu` (form above), on `pull_request`, schedule and `workflow_dispatch` (no push trigger); fork PRs skip it. Its status job `gs-combined-1` runs on GitHub-hosted `ubuntu-latest`, `needs:` the GPU job and has `if: ${{ always() }}` as its only job-level condition, so it always reports: a skipped GPU job (a fork PR) fails the status with `gs_combined_requires_trusted_branch` instead of satisfying it, and a cancelled or failed one fails it too. `tests/ci/test_release_builds_runner_trust.py` (`GsCombinedGateShapeTests`) enforces the guard, the status job's runner, its `needs:`, its condition and its fail-closed step, with a mutation per property.
- `release_builds.yml` — self-hosted jobs `export_smoke_linux` (strict), `export_smoke_windows` (strict). The tag after each job is the guard form it actually carries, and `tests/ci/test_release_builds_runner_trust.py` compares it against the workflow **per job**, so this line cannot go on claiming a form a job has stopped using. **strict** = `if: github.event_name != 'pull_request'`, which skips **all** pull requests (fork *and* same-repo); **standard** = the repository-standard fork guard in the code block above, under which trusted same-repo PRs still run. `export_smoke_linux` is the blocking export smoke of the release gates: it exports the test project against the Linux release template built by the same run and then *launches* the exported binary on the RTX 3080 (real Xorg at `DISPLAY=:0`), and `export_smoke_probe.gd` requires a live RenderingDevice and a real window read-back. `export_smoke_windows` is the Windows counterpart and needs Windows *and* a GPU, which neither a GitHub-hosted Windows runner (no GPU) nor the Linux runner (cannot run a Windows exe) provides; it routes to `[self-hosted, Windows, X64, gpu, d3d12]` and is **skipped unless the repository variable `GODOTGS_WINDOWS_GPU_RUNNER` is `true`** (set it once such a runner is registered; without it the job would queue for about 24 h and hold later release runs behind it). It is deliberately outside every `needs:` list and every publication `if:`, so it blocks nothing either way. Its contract (same script, `--target windows`, same negative control) is kept intact rather than deleted. `build_windows` and `build_windows_export_template` run on GitHub-hosted `windows-latest`, so they are outside this trust boundary (they still skip every pull request, as a cost decision). **The consequence is stated plainly:** the Windows editor zip and the Windows export template zip still ship, but no lane executes the Windows exe; only the Linux template is executed before publication. The `export_smoke_windows` guard is the strict form with the repository variable ANDed in front (`vars.GODOTGS_WINDOWS_GPU_RUNNER == 'true' && github.event_name != 'pull_request'`); a conjunction can only narrow what the strict form admits. The deviation from the standard guard is *narrower*, never wider — it cannot admit fork code — but it does cost pull-request coverage, which is accepted deliberately: the Windows-specific *naming* logic that actually broke (#825, the `.console.exe` wrapper name) lives in `tests/ci/resolve_export_template.py` and is unit-covered on every PR by `tests/ci/test_resolve_export_template.py`, and `build_linux_export_template` is GitHub-hosted, runs every night (it no longer runs on pull requests since the CI diet), and drives the same resolver and package/checksum/upload shape. Kept in sync with the workflow by `tests/ci/test_release_builds_runner_trust.py`, which derives the self-hosted job set from `release_builds.yml` — by label routing, so a job that reaches the persistent runner through its custom labels alone (no `self-hosted` label) is caught too — and fails if a job here is undocumented, documented but nonexistent, carrying neither accepted guard form, or carrying a different form than the tag above claims.

### Runner label policy

Which jobs sit inside the trust boundary is decided by **label routing**: GitHub
sends a job to any runner carrying *all* of its `runs-on:` labels, and the
`self-hosted` label is conventional, not required. So the guard needs to know
which labels reach the persistent runner and which do not — and that is a fact
about the runner inventory, not something that can be read out of the workflow
text. It is therefore **declared here** and reviewed by a maintainer:

- Persistent self-hosted runner labels: `self-hosted`, `Linux`, `Windows`, `X64`, `gpu`, `rtx3080`, `d3d12`
- GitHub-hosted runner labels: `ubuntu-latest`, `ubuntu-24.04`, `windows-latest`

Those two bullets are parsed, so their **form is load-bearing**. Each must open
with its clause (`- <clause>:`) and then contain a comma-separated list of
backticked labels and nothing else — an optional closing period is the only
extra allowed. Do not append an example, a counter-example or an explanation to
either bullet, and do not restate either clause in a second bullet: every
backticked token in the declaration is read as current policy, so a
counter-example such as `windows-2022` written inside the second bullet would
*become* a declared GitHub-hosted label and quietly move a job out of the trust
boundary. Explanations belong in paragraphs like this one. A bullet that only
mentions a clause mid-sentence (“we no longer declare …”) is not a declaration
and is rejected, rather than being read backwards.

`tests/ci/test_release_builds_runner_trust.py` parses both bullets and
cross-checks them: every label that appears next to `self-hosted` in any
`.github/workflows/*.yml` must be listed above (so a label newly added to the
runner fails the guard until it is declared), and the two lists must be
**disjoint**. That scan is case-insensitive, exactly as GitHub's own label
matching is, so `runs-on: [Self-Hosted, …]` contributes its labels just like the
lowercase spelling. A `runs-on:` whose labels are neither a subset of the
persistent list nor a *single* label from the GitHub-hosted list is a hard
failure, not an assumption of safety.

Two label shapes are deliberately **not** treated as GitHub-hosted, because
earlier versions of the guard inferred hosting from the shape of the label text
and were wrong:

- an image-*looking* label that is not declared above (`windows-2022`,
  `macos-14`, …). Nothing stops a self-hosted runner from carrying such a label
  as a custom label — `windows-2022` is the natural one for a self-hosted Windows
  Server 2022 machine — so the string tells you nothing about where the job lands;
- **any** multi-label set of image-looking labels, e.g.
  `runs-on: [ubuntu-latest, windows-2022]`. No GitHub-hosted runner carries two
  image labels, so the only machine that can match all of them is a self-hosted
  one. A job in that shape used to be classified GitHub-hosted and therefore
  received neither the fork-guard check nor the documentation checks below.

Adding a label to this list is the point at which a human asserts it is absent
from the self-hosted runner inventory. Do not add one to make a check pass.

`pull_request_target` is not used by any workflow, so fork PRs never get a privileged
checkout. A fork PR's GPU/Windows validation happens only after a maintainer reviews
the change and moves it onto a same-repo branch.

**Merge queue (`merge_group`).** These workflows also trigger on `merge_group`, where
the self-hosted jobs run. Only users with write access can add a PR to the merge
queue, so queued content is **maintainer-gated**: a queued fork PR's code would run on
the self-hosted runner, which is an accepted maintainer-gated property (the same trust
as merging). Restricting `merge_group` to same-repo-only queued PRs via a head-repo
preflight is a possible future hardening; it is out of scope here, which keeps this
change focused on the fork-`pull_request` boundary.

Any change that relaxes this boundary must be documented here and approved by a
maintainer (see the project governance docs under `docs/governance/`).

## Self-hosted GPU runner environment

The only self-hosted runner is `gpu-linux-3080`, a dedicated Linux machine with
an RTX 3080 (the earlier Windows runner was the maintainer's workstation, which
is where the findings below were measured, on Windows and an RTX 3090). The
mechanisms in this section were written for that Windows machine and still
hold: the environment is **controlled per job and verified at runtime** rather
than assumed. Linux differences:

- Godot renders on the real Xorg of the NVIDIA GPU (`DISPLAY=:0`,
  `--display-driver x11`); the runner `.env` pins `VK_ICD_FILENAMES` to the NVIDIA
  ICD, and Xvfb is not used (no NVIDIA Vulkan).
- The Linux Vulkan loader (1.3.x) prints `INFO | LAYER: Insert instance layer ...`
  without the `[Vulkan Loader]` marker; the preflight parses both formats. The
  NVIDIA Linux driver inserts no implicit layers by default, so the effective
  chain must be **empty**, and the one implicit layer the distro registers
  (`VK_LAYER_MESA_device_select`) is stripped by its own opt-out
  `NODEVICE_SELECT=1`. The control run must still show a layer, which is what
  keeps an empty effective chain from being a parser that matches nothing.
- There is no page heap / IFEO on Linux; gate 2 reports N/A.
- Contention is measured from `nvidia-smi pmon` (per-pid SM share) joined
  against `/proc` for the image path. The headless Xorg that presents the job's
  windows is counted as the job's own load.
- Builds use `-j16`, not `nproc`: the machine has 24 GB of RAM.

The Windows text that follows is kept because the `d3d12` Windows workstation
(`export_smoke_windows`) runs the same preflights.

The settings below have already produced misleading results (#874, #875).

### Third-party Vulkan implicit layers

Seven third-party implicit layers are registered machine-wide under
`HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers` (and the `WOW6432Node` view):
RenderDoc, the Steam overlay, Steam Fossilize, the Epic Online Services overlay,
the OBS studio hook, and Overwolf's overlay and graphics hooks. A layer in the
chain changes allocation behaviour, command-buffer handling, pipeline creation
and timing even when it is not capturing, and one of them is already in our
logs: PR #852's run 31487454038 shows the loader failing to resolve
`vkGetInstanceProcAddr` in Overwolf's `ow-graphics-vulkan.dll` while building a
device chain, in the same job that then could not create a `RenderingDevice`.

The registry is not CI's to edit and the software is not CI's to uninstall, so
every GPU-pool job exports, at job level:

```yaml
env:
  # Loader-side filter. Works only in a non-elevated process — see below.
  VK_LOADER_LAYERS_DISABLE: '~implicit~'
  VK_LOADER_LAYERS_ENABLE: 'VK_LAYER_NV_optimus,VK_LAYER_NV_present'
  # Each layer's own opt-out. This is what actually works on this runner.
  DISABLE_VULKAN_OBS_CAPTURE: '1'          # VK_LAYER_OBS_HOOK
  DISABLE_VULKAN_OW_OBS_CAPTURE: '1'       # VK_LAYER_OW_OBS_HOOK
  DISABLE_VULKAN_OW_OVERLAY_LAYER: '1'     # VK_LAYER_OW_OVERLAY
```

#### Why two mechanisms, and why the obvious one is not enough

Measured as the interactive user, `VK_LOADER_LAYERS_DISABLE=~implicit~` works
exactly as documented: the loader inserts no implicit layers at all, and adding
`VK_LOADER_LAYERS_ENABLE` brings back the two named NVIDIA layers, enable taking
precedence over disable on loader `1.4.341.0`.

**In the GPU jobs it does nothing.** The runner service
(`actions.runner.klausi3D-godotGS.DESKTOP-NLG4NKL-godotgs`) is registered with
`StartName: LocalSystem`, so every job step runs as `NT AUTHORITY\SYSTEM` at
System integrity (`S-1-16-16384`). The Vulkan loader reads its own filter
variables through `loader_secure_getenv`, which discards them in an elevated
process — and says so, at `VK_LOADER_DEBUG=info`:

```
[Vulkan Loader] INFO: Loader is running with elevated permissions.
                      Environment variable VK_LOADER_LAYERS_DISABLE will be ignored
```

Run 31603970211 on this runner probed the loader under five environments in the
job context. `~implicit~`, `~implicit~` + enable, `~all~`, and no variables at
all produced the **identical** chain; `VK_LAYER_PATH`, `VK_ICD_FILENAMES`,
`VK_LOADER_DRIVERS_DISABLE` and eleven other variables are discarded the same
way. `VK_LOADER_DEBUG` itself is read unprivileged, which is the only reason any
of this is observable.

What does work in that context is the per-layer opt-out each layer declares in
its **own** manifest under `disable_environment` — the loader reads those with
the ordinary getenv. In the same run, setting those three and nothing else
reduced the System-integrity chain to the driver's own two layers. So both
mechanisms are exported: the per-layer variables are what strips the layers
here, and the loader filter remains for the layers nobody has enumerated and for
any runner whose service is not elevated. Neither is trusted — the preflight
below measures the result.

Only three of the seven were ever actually in the chain. RenderDoc, both Steam
layers and the EOS overlay declare an `enable_environment` key in their layer
JSON, so the loader skips them unless their opt-in variable is set; the OBS hook
and Overwolf's two layers declare only a `disable_environment` and inject
unconditionally. The registry therefore over-reports the problem, which is
precisely why the check below reads the loader rather than the registry.

### The preflight

**Setting an environment variable is not evidence.** A value the loader does not
understand — or a correct value in a process the loader declines to read it in,
which is exactly what happened above — is ignored in silence, and the job stays
green with every layer still in place. That is not hypothetical: the first
version of this change set the loader-filter variables, was verified
interactively, and was caught by this preflight because it changed nothing in
the job (#878). So each GPU-pool job runs, before its build and before any GPU
step:

```yaml
- name: Preflight - runner GPU environment (#875)
  run: python tests/ci/preflight_runner_gpu_environment.py
```

`tests/ci/preflight_runner_gpu_environment.py` never reads the environment variable
and never reads the registry. It runs a probe process under
`VK_LOADER_DEBUG=layer,info` and parses the loader's own `Insert instance layer` /
`Inserted device layer` messages, twice: once with every disable variable
stripped (the *control*), once with the job's environment as-is (the
*effective*). It fails if the effective chain holds any layer that is not the
GPU driver's own, naming the layer and the module the loader loaded for it; it
fails if any of the driver's own layers present in the control run went
*missing* from the effective one, because an empty chain contains nothing
unexpected while meaning the GPU jobs moved to a driver stack nothing else uses.
That survival check is **per chain**, not per layer name: the loader builds an
instance chain and a device chain and reports them separately, so a layer that
survives on one and vanishes from the other keeps its name in the effective set
while the device stack the GPU jobs measure on has changed — a name comparison
cannot tell that output apart from a healthy run.
And it fails if the **control** run reports no layers at all, because a parser
that has stopped matching and a machine with no layers would otherwise look
identical. It also reports any filter variable the loader says it discarded for
elevation, so a control and effective run coming back identical arrives with its
explanation attached. The same script asserts that the page-heap / Application
Verifier IFEO flags found on this runner (#874) stay removed — failing closed on
a registry read that does not succeed, rather than reading "could not look" as
"not set" — and records GPU occupancy at job start.

A layer this preflight reports is a finding to act on, not one to add to its
allowlist. Widening `EXPECTED_LAYERS` asserts that a layer is part of the GPU
driver and cannot be removed — a claim about the machine that a maintainer makes.

### GPU contention

The runner is the maintainer's workstation and there is only one of it. That is
an **accepted constraint**, not something CI can fix — so the handling is
attribution, never tolerance.

Unrelated GPU work lands on the box while jobs run. On the day #875 was written
it happened three times: 4× `Godot_v4.7-stable` gdUnit4 at 11:13, the same again
around 13:00, 2× `Godot_v4.5.2-stable` at 22:28. None were ours; all contend for
the GPU the runner uses. The cost is not slow jobs, it is **false failures that
read like real ones**: PR #881's streaming gate failed on wall-clock budgets
alone (`first_visible_ms=3500`, `frame_p95_to_avg_ratio=1.935`) while residency
hit 1.0, fallback rate 0.0 and readiness `READY` — nothing failed functionally.
#867's `NodeSceneTree` 300 s timeout was the same thing and cost a full
diagnosis cycle.

Every GPU-pool job therefore runs, before its build:

```yaml
- name: Preflight - GPU contention, wait for a free runner (#875)
  run: python tests/ci/runner_gpu_contention.py preflight
```

and, as its last step, **with `if: always()`**:

```yaml
- name: Postflight - GPU contention verdict (#875)
  if: always()
  run: python tests/ci/runner_gpu_contention.py postflight
```

**It waits; it does not fail on sight.** On a machine that is also somebody's
workstation, failing the moment the GPU is busy would let the owner's own work
block CI outright. The preflight polls for the GPU to be free of *foreign* load
(our own build and tests are supposed to use it) for up to **15 minutes**, which
is longer than any comparable workload of ours — the GPU harness runs 3–8
minutes per batch — and is 12.5 % of the 120-minute `gpu-tests` timeout, so a job
that waits the whole bound still keeps over 105 minutes and cannot fail *as a
timeout* because of the wait. Beyond a quarter of an hour the contending work is
not a transient; it is someone using their machine, and the right answer is a
loud "retry when it is free" rather than CI sitting on a serialised queue.

**When it does give up, it is unmistakable.** Exit code **75**, not 1 — a job
that ends there did not fail a test, it produced no measurement — behind a
banner reading `RUNNER BUSY — THIS RESULT IS VOID`, naming every process holding
the GPU with pid, image path and measured share, plus a GitHub error annotation
and a step-summary entry.

**Start *and* end, and everything between.** #881's interference began *after*
the job started, so a check that only looked at job start would have called that
run clean. The preflight leaves a detached sampler running for the life of the
job; the postflight stops it, takes a closing measurement, and reads back the
whole series. A run that was clean at start and contended while running is
`CONTENDED_MID_RUN` — void, with the window and the offending image printed. A
run whose series has a hole larger than five minutes is `UNMEASURED` — also
void, because "the monitor saw nothing" and "the monitor was not running" must
never read the same.

**"It was monitored" is a positive fact, not the absence of a complaint.** The
postflight appends its own closing sample to the same series, so a series
containing *only* that sample would otherwise score as perfect continuous
coverage over a zero-length window — a measurement of the last instant standing
in for a measurement of the job. Every sample therefore records who wrote it, and
only samples written by *this job's* sampler (matched by the pid the preflight
got back from the spawn) count as coverage; the preflight in turn declares
monitoring active only once that sampler has actually written its first sample,
not merely when it was started. Contention, by contrast, is read from every
sample whoever wrote it: filtering evidence on provenance could only turn a void
run green.

**An orphaned sampler is stopped, never inherited.** A job killed by its own
timeout never runs its postflight, so its detached sampler keeps going for up to
three hours. Before reusing the shared record directory, the next preflight
terminates that sampler — but only after confirming from the process's command
line that the pid really is this guard's sampler for this directory. Pids are
recycled and this runner is also the maintainer's workstation, so an
unidentifiable or reassigned pid is left alone and recorded rather than killed.

**Nothing is relaxed.** No budget, timeout or threshold moves. A contended run is
*void*, never *passed*. Every verdict also prints the discriminator this repo
already measured — clean `frame_p95_to_avg_ratio ≈ 1.15` on the streaming lane
(#630/#624) — so a reader who arrives at a bare `first_visible_exceeded` sees
"ratio 1.94, this is contention" instead of starting a diagnosis from scratch.
Whether wall-clock budgets belong in correctness lanes at all is #523/#778 and a
much larger change.

On Linux the per-process source is `nvidia-smi pmon -c 1 -s u` (per-pid SM
share) joined against `/proc` for the image path; the rest of this paragraph
describes the Windows backend. Busy is decided from two independent sources: the Windows
`\GPU Engine(*)\Utilization Percentage` counters, whose instance names carry the
owning pid, joined against `Win32_Process` for the image path (this is what makes
a failure *actionable* — it names the process); and `nvidia-smi` as the driver's
aggregate view, recorded always. `nvidia-smi --query-compute-apps` is
deliberately not the attribution source: under WDDM it lists 60 rows of shell,
browser and tray processes with `[N/A]` memory — volume without attribution. The
15 % threshold sits above the measured idle desktop floor on this machine (0.5–5.5 %
per process, aggregate 3–7 %) and well below what a single Godot workload pulls
(19–34 %, measured).

### Which jobs

The GPU pool is derived, never listed here:
`tests/ci/test_preflight_runner_gpu_environment.py` takes every self-hosted job
in `.github/workflows/` — **both** the `*.yml` and `*.yaml` suffixes GitHub
reads, because a job in an `x.yaml` workflow runs exactly like one in `x.yml`
and a guard that scans only one of them reports "no such job" over a real
self-hosted GPU job — carrying the `gpu` label, reusing the suffix set *and* the
label-routing classification from
`tests/ci/test_release_builds_runner_trust.py` — and requires
each to export both the loader-filter pair *and* every per-layer opt-out at job
level, to run the preflight, and to run it
before the build. `tests/ci/test_runner_gpu_contention.py` reuses that same
derivation — importing it rather than restating it, so the two guards cannot come
to disagree about which jobs are the GPU pool — and requires each derived job to
run the contention preflight before its build, to run the postflight after it,
and to carry `always()` on the postflight (a verdict that is skipped when a step
above failed is missing from the only runs anyone reads it in). An empty derived
set fails both guards rather than passing. At
the time of writing that set is `gpu-tests`
(`baseline_qa.yml`), `openworld-proof-evidence`
(`gaussian_production_gates.yml`), and `runtime-release-ci`
(`release_ci_runtime.yml`), and `prime-world-cache`, `prime-auto-tree` and `flythrough` (`beyond_vram_flythrough.yml`), all on the Linux RTX 3080 runner, plus
`export_smoke_linux` (`release_builds.yml`), also on the Linux runner, plus
`export_smoke_windows` (`release_builds.yml`), which is skipped unless
`GODOTGS_WINDOWS_GPU_RUNNER` is `true` and a Windows GPU workstation labelled `d3d12` exists.

The self-hosted jobs *without* the `gpu` label — `shader-validation`
(`gaussian_shader_validation.yml`), the three `build-linux-editor` jobs
(`gaussian_production_gates.yml`, `release_ci_runtime.yml`,
`beyond_vram_flythrough.yml`) and the CPU-only BV-C2 `codec-probe`
(`beyond_vram_flythrough.yml`) —
may run on the Linux GPU runner (they match its `self-hosted, Linux, X64` labels)
but do not create a Vulkan device, so they are deliberately out of scope. The
GPU jobs those build jobs feed have no SCons step, so their ordering anchor is
declared in `GPU_WORK_ANCHORS` (the first step that launches the binary).
`baseline_qa.yml` `gpu-tests` builds in-job, so its SCons step is its anchor. (`build_windows` and `build_windows_export_template`
are GitHub-hosted now.) If one of them grows a GPU step it must also gain the `gpu` label,
which brings it into the derived set automatically.

## Build and GPU-test split

**Per PR: one build, in the GPU job (CI diet, 2026-11-07).** `baseline_qa.yml`
`gpu-tests` compiles the `tests=yes` editor itself from the runner's persistent
object cache (`/opt/actions-runner/godotgs-scons-cache`) and tests that binary in
the same job. Before, every PR built the same configuration twice (Baseline QA and
Production Gates `build-linux-editor`) and moved it through two artifact uploads
and two or three downloads to the same machine: measured about 8.5-10 of 26-33
runner-minutes per PR. A cache-hit build takes about 2 minutes; a download alone
took 2.2-3.5. Splitting build and test across a CPU and a GPU runner on the same VM
would still pay the upload and the download, so it is not cheaper per PR; the split
below stays for the weekly and nightly lanes. No binary crosses a job boundary in
`gpu-tests`, so there is no provenance hand-off to verify; its checkout is
`github.sha`. The CPU-only lanes build their own editor on a GitHub-hosted runner
(`cpu-tests`), so they never occupy the GPU runner.

`gaussian_production_gates.yml` and `release_ci_runtime.yml` each compile their
`tests=yes` Linux editor in a `build-linux-editor` job that runs on
`[self-hosted, Linux, X64]`, **without** the `gpu` label, so a CPU-only runner can
take the job once one is registered.

- **Pinned to the commit.** `tests/ci/linux_gpu_runner.sh stage-binary` refuses
  unless the checkout it built is `github.sha`, and writes `build-provenance.txt`
  (commit, run id, binary name, sha256) beside the binary. The artifact is named
  `godot-linuxbsd-editor-tests-<github.sha>`.
- **Verified before use.** Each GPU job `needs:` the build job, downloads that
  artifact, and runs `linux_gpu_runner.sh install-binary`, which fails closed
  unless the provenance names the job's own `github.sha` and run id, the job's
  checkout is that commit, the artifact holds nothing else and the sha256
  matches. Only then is the binary placed at `bin/<name>` for the unchanged
  "Resolve Godot binary" step. The GPU jobs compile nothing.
- **Same build, same gates.** The SCons arguments are the ones each GPU job used
  to run, moved verbatim, so build flags, the binary and the object cache in
  `/opt/actions-runner/godotgs-scons-cache` (SCons `cache_path`, the compile
  cache these lanes already have) are unchanged. GPU-job names, timeouts,
  preflights, postflights and gates are unchanged; the job names that still say
  "Build" are status-check contexts.
- **The beyond-VRAM fly-through lane uses the same handoff with a different
  build.** `beyond_vram_flythrough.yml`'s `build-linux-editor` builds the
  optimized producer binary (`dev_build=no optimize=speed_trace`, ADR D-10) into
  its own cache `/opt/actions-runner/godotgs-scons-cache-opt` (`cache_limit=15`,
  the ADR section 5.3 budget line), so it never evicts the dev lanes' cache. Its
  binary is `godot.linuxbsd.editor.x86_64`, the non-`.dev` name both handoff
  halves already accept; `prime-world-cache`, `prime-auto-tree` and `flythrough` resolve only that name and their
  `build_info` step fails if the SCons arguments it records are not the D-10
  producer flags.
- **A failed build is red, not hidden.** The GPU jobs are skipped behind a red
  `build-linux-editor`; none of them is a required check. On the
  `openworld-proof-evidence` events the build job does not inherit that job's
  `continue-on-error`, so a compile failure there is a red run instead of an
  absorbed one. In `baseline_qa.yml` `gpu-tests` a failed build fails the job at
  its build step.

## Scheduled Triggers

| Workflow | Schedule (UTC) | Behavior |
| --- | --- | --- |
| `gaussian_shader_validation.yml` | `15 0 * * *` | Re-validates the shader matrix on master HEAD without a path filter (replaces the former push trigger). |
| `baseline_qa.yml` | `30 0 * * *` | The nightly full run: every path filter off, the full guard phase including every guard self-test (`cpu-tests`), every doctest lane, every GPU lane including both slow streaming scenarios and the production evidence collector. The GPU harness runs in update mode and publishes the `gpu-harness-recaptured-baselines` artifact (recaptured PNGs + provenance); it opens a recapture PR when `BASELINE_UPDATE_PAT` is provisioned. Never cancelled by a later run. |
| `gaussian_production_gates.yml` | `30 3 * * 1` | Runs two non-blocking benchmark evidence surfaces in the `openworld-proof-evidence` job: `openworld-proof-dev` (`open_world_corridor_proof` + `city_flyover`, performance profile) and `openworld-proof-weekly`. A `workflow_dispatch` run executes each surface only when its own input (`run_openworld_proof_dev` / `run_openworld_proof_weekly`) is set. |
| `release_builds.yml` | `30 2 * * *` | Builds and publishes the nightly prerelease, then prunes older nightly releases and tags. |
| `release_ci_runtime.yml` | `0 6 * * *` | Runs the non-blocking `release-ci` runtime evidence lane on the self-hosted Linux GPU runner and uploads the runtime validation report. |
| `gs_combined_1.yml` | `30 7 * * *` | Runs the GS-COMBINED-1 gate (both variants) on the self-hosted Linux GPU runner; reported, not required. |
| `codeql.yml` | `0 4 * * 0` | Weekly CodeQL analysis on GitHub-hosted runners (no self-hosted time). |

## Dependencies

- Python 3.11
- PyYAML 6.0.2 for workflow parsing in `agentic-pr-gate`, installed from the
  CI-only version-and-hash pin in `tests/ci/requirements-automation.txt`
- SCons/build toolchain for compiled lanes. Every `pip install` in CI uses
  `--require-hashes` against a version-and-hash-locked file (GAP-403). Each
  lock records its pip-tools regeneration command in its header.
  - `tests/ci/requirements-scons.txt` (SCons 4.10.1, compiled from
    `requirements-scons.in`) is for the build steps in `baseline_qa.yml` (`gpu-tests`), `gaussian_production_gates.yml`,
    `release_builds.yml` (the GitHub-hosted Windows builds) and
    `release_ci_runtime.yml`. On the self-hosted Linux runner those jobs never
    install into its global Python: `tests/ci/linux_gpu_runner.sh venv` creates a
    venv under `$RUNNER_TEMP`, installs the lock there, and prepends the venv's
    `bin` directory to `GITHUB_PATH`, so every later `python`, `python -m SCons`
    included, runs in the venv. The self-hosted jobs with no install step
    (`export_smoke_windows` and the job in `gaussian_shader_validation.yml`)
    use the global interpreter but install nothing into it.
  - `tests/ci/requirements-test-data.txt` (numpy, compiled from
    `requirements-test-data.in`) is for the hosted `cpu-tests` job in
    `baseline_qa.yml`, which takes its SCons from apt (and runs the full guard
    phase nightly), for the hosted `agentic-pr-gate`, whose guard-only lane runs
    the S1b harness unit tests, and for `baseline_qa.yml` `gpu-tests`, whose
    evidence collector runs `tests/ci/run_module_tests.py`: there
    `tests/ci/linux_gpu_runner.sh venv --with-test-data` installs it into the
    same per-job venv as SCons.
  - `docs/requirements-lock.txt` is for `docs_pages.yml`. It holds the full
    transitive docs stack, compiled from `docs/requirements.txt` and
    `docs/requirements-site.txt`.
  - The hosted Linux jobs in `release_builds.yml` take their SCons from apt and
    install nothing from PyPI.
- Self-hosted Linux GPU runner `gpu-linux-3080` (labels `self-hosted`, `Linux`, `X64`, `gpu`, `rtx3080`): Ubuntu 24.04, NVIDIA driver with Vulkan, a real Xorg at `DISPLAY=:0`, no sudo. Jobs never install system packages; `tests/ci/linux_gpu_runner.sh check-tools` fails closed and names what is missing
- Optional Windows GPU workstation with labels `self-hosted`, `Windows`, `X64`, `gpu`, `d3d12`, for `export_smoke_windows` only
- Vulkan-capable environment for render-path lanes
- `xvfb` is used by the hosted Linux CPU lane only; the self-hosted GPU jobs render on the real Xorg (Xvfb has no NVIDIA Vulkan)

## Archived Workflows

Disabled workflows are stored in `../archived-workflows/`.

- `benchmark.yml.disabled`
- `build-engine.yml.disabled`
- `gaussian_pipeline_validation.yml.disabled`
- `test_gaussian_splatting.yml.disabled`
- `test_phase4.yml.disabled`
