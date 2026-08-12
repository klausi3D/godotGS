# GitHub Actions Workflows

This directory contains 7 active workflow files.

GitHub's Actions tab can also show historical workflow names from past runs, disabled files, or workflow files that are no longer present in this directory. This README tracks the workflow files currently checked into `.github/workflows/`.

## Active Workflows

| Workflow | File | Purpose | Notes |
| --- | --- | --- | --- |
| Baseline QA Automation | `baseline_qa.yml` | Runs baseline QA, the blocking QA-scene visual gate, the golden-image GPU harness, and optional compiled-module QA. | Builds the Linux editor once and reuses that artifact for push-only compiled QA. The `gpu-tests` job runs the `qa` scene category on the real display with `--qa-require-capture --require-qa-baseline` (#522) — headless cannot create a RenderingDevice, so the scenes would capture nothing and the category would degrade to a skip. That step is compare-only and never rewrites its own baseline. |
| Docs Pages (Versioned) | `docs_pages.yml` | Builds and deploys MkDocs docs with mike versioning to `gh-pages`. | Publishes `latest` from `master/main` and versioned docs from `v*` tags. |
| Gaussian Production Gates | `gaussian_production_gates.yml` | Enforces guard checks, pipeline smoke, runtime validation, the blocking streaming gate, and optional non-blocking benchmark evidence surfaces. | Owns the single Windows build for validation workflows. `streaming-gpu-ci` is the canonical blocking GPU-backed streaming runtime gate; `openworld-proof-dev` and `openworld-proof-weekly` are evidence-only benchmark surfaces. |
| Gaussian Shader Validation | `gaussian_shader_validation.yml` | Validates shader compile matrix and host/shader contract checks. | Focused shader CI gate. |
| Release Builds | `release_builds.yml` | Builds Linux and Windows editors for CI artifacts, nightly prereleases, and stable-tag publishes, plus the Linux and Windows `target=template_release` export templates. | Publishes Linux tarballs and Windows zips on the nightly schedule and on `v*` tag pushes. The `finite_math_guard` job blocks publication on every channel, and the `release_candidate_gate` job gates the stable/tag publish path (both-platform builds + `--mode candidate` validation, fail-closed); see below. The `build_linux_export_template` / `build_windows_export_template` jobs (#825) upload export templates as **artifacts only** — they are deliberately not wired into `release_candidate_gate` or `publish_release`; see [export templates](../../docs/development/export-templates.md). |
| Agentic PR Gate | `agentic_pr_gate.yml` | Fork-safe, always-on gate: validates the agentic control plane, runs the agentic tests, the agentic/governance link check, and the GPU-free `--guard-only` lane. | GitHub-hosted (`ubuntu-latest`); runs on every PR and the merge queue. Required status check (job name): `agentic-pr-gate`. |
| Release-CI Runtime Evidence | `release_ci_runtime.yml` | Nightly + manual evidence lane for the canonical release-ready runtime profile `release-ci` (non-headless GDScript runtime suite + required renderer proof). | Self-hosted Windows GPU runner. **Not a required PR gate** — schedule + `workflow_dispatch` only. Runs `run_runtime_validation.py --profile release-ci --gd-mode windows-vulkan --skip-cpp`. |

## Required Checks

`agentic-pr-gate` (the job name in `agentic_pr_gate.yml`, shown in the PR checks UI
as `Agentic PR Gate / agentic-pr-gate`) is the fork-safe, always-on blocking check
intended for `master` branch protection (see `docs/governance/github-settings.md`,
added by a sibling PR in this foundation series).
It runs only on GitHub-hosted runners, so external fork PRs always receive a status
without touching the self-hosted lanes. It runs:

- `python scripts/agentic/validate_repo_contract.py`
- the `scripts/agentic` contract validators against the shipped templates
- `python -m unittest discover -s tests/agentic`
- `python scripts/docs/check_links.py docs README.md BUILDING.md CONTRIBUTING.md AGENTS.md CLAUDE.md`
- `python tests/ci/run_module_tests.py --guard-only` (GPU-free; the StringName guard
  self-skips when no Godot binary is present)

The link check covers the full docs tree plus the root governance docs (only paths
present in the tree are passed, so it is robust on partial trees and is the full-docs
check on `master`).

## Manual Dispatch Inputs

| Workflow | Input | Options |
| --- | --- | --- |
| `baseline_qa.yml` | `debug_mode` | `true`, `false` |
| `baseline_qa.yml` | `baseline_mode` | `compare`, `update` |
| `gaussian_production_gates.yml` | `run_gpu_lane` | `true`, `false` |
| `gaussian_production_gates.yml` | `run_openworld_proof_dev` | `true`, `false` |
| `gaussian_production_gates.yml` | `run_openworld_proof_weekly` | `true`, `false` |
| `gaussian_production_gates.yml` | `enforce_gpu_readiness` | `true`, `false` |
| `gaussian_production_gates.yml` | `runtime_loops` | integer string |
| `release_builds.yml` | `publish_channel` | `none`, `nightly`, `stable` |
| `release_builds.yml` | `release_tag` | string (`vX.Y.Z` when `publish_channel=stable`) |
| `release_builds.yml` | `release_name` | optional string |
| `release_builds.yml` | `keep_nightlies` | integer string |

## Renderer Release Gate Contract

The renderer/public-alpha evidence policy is maintained in
`docs/reference/renderer_release_gate_manifest.json` and validated with:

```bash
python tests/ci/check_renderer_release_gates.py --mode contract
```

The same contract check is part of `tests/ci/run_module_tests.py --guard-only`,
which is what the Gaussian Production Gates `guards` job runs. The contract check
is deterministic and GPU-free. Public-alpha candidate mode
requires the evidence bundle, a public-alpha channel/tag selector, and a live
issue-label snapshot so P0, P1, and release-blocker issues cannot be bypassed by
release notes or manual workflow choices. The workflow-policy
portion of the checker validates required workflow files and job markers only;
the stronger no-downgrade workflow rules remain documented review policy until
the checker grows a real GitHub Actions behavior parser.

External checks are not automatically renderer release blockers. `qlty check`
is currently documented in the manifest as a deferred, non-blocking external
signal because `master` branch protection has no required status checks and the
repo does not track a qlty configuration/log contract. If branch protection
later requires qlty, update the manifest before treating a qlty result as part
of public-alpha signoff.

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
  public-alpha evidence bundle and **fails closed** when the bundle is absent, so
  a tag cannot publish without passing candidate validation;
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
cutting a candidate points the `RELEASE_CANDIDATE_EVIDENCE` (and optional
`RELEASE_CANDIDATE_ISSUES`) repo/environment variable at a produced bundle. See
`docs/reference/renderer-release-gates.md` for details.

## Runner Trust Boundary (fork PRs)

The self-hosted Windows/GPU runners are persistent and must never execute
untrusted code from a **fork pull request**. The self-hosted jobs carry a fork guard
so fork PRs are skipped on those runners, while same-repo (maintainer) PRs, `push`,
and `workflow_dispatch` still run:

```yaml
if: ${{ github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository }}
```

- `baseline_qa.yml` — `gpu-tests`, `gpu-harness` (form above).
- `gaussian_production_gates.yml` — `guards`, `module-validation` (form above), and `openworld-proof-evidence`, which carries a *narrower* guard: `if: github.event_name == 'schedule' || (github.event_name == 'workflow_dispatch' && ...)`, so it never runs on a pull request at all, fork or same-repo.
- `gaussian_shader_validation.yml` — `shader-validation` (form above).
- `release_ci_runtime.yml` — `runtime-release-ci` (form above). This workflow has no `pull_request` trigger (schedule + `workflow_dispatch` only), so the guard is trivially satisfied; it is carried explicitly to keep the self-hosted job fail-closed if a `pull_request` trigger is ever added.
- `release_builds.yml` — self-hosted jobs `build_windows` (strict), `build_windows_export_template` (strict). The tag after each job is the guard form that job actually carries, and `tests/ci/test_release_builds_runner_trust.py` compares it against the workflow **per job**, so this line cannot go on claiming a form one of them has stopped using. **strict** = `if: github.event_name != 'pull_request'`, which skips **all** pull requests (fork *and* same-repo); **standard** = the repository-standard fork guard in the code block above, under which trusted same-repo PRs still run. Both Windows release lanes therefore run on `push`/tag/schedule/dispatch only. The deviation is *narrower* than the standard form, never wider — it cannot admit fork code — but it does cost pull-request coverage, and that cost is accepted deliberately rather than overlooked: Windows-only packaging steps (PowerShell staging, zip, checksum) are first exercised after the branch reaches `master`, or on the nightly/dispatch run. Two things bound the exposure. The Windows-specific *naming* logic — the part that actually broke (#825, the `.console.exe` wrapper name) — lives in `tests/ci/resolve_export_template.py` and is unit-covered on every PR by `tests/ci/test_resolve_export_template.py`; and `build_linux_export_template` is GitHub-hosted, runs on pull requests, and drives the same resolver and the same package/checksum/upload shape. Moving these jobs to the standard guard would place a multi-hour template build on the single shared self-hosted runner ahead of the GPU gates on every same-repo PR, so it is a maintainer trade-off rather than a default. Kept in sync with the workflow by `tests/ci/test_release_builds_runner_trust.py`, which derives the self-hosted job set from `release_builds.yml` — by label routing, so a job that reaches the persistent runner through its custom labels alone (`runs-on: [Windows, X64, godotgs]`, no `self-hosted` label) is caught too — and fails if a job here is undocumented, documented but nonexistent, carrying neither accepted guard form, or carrying a different form than the tag above claims.

### Runner label policy

Which jobs sit inside the trust boundary is decided by **label routing**: GitHub
sends a job to any runner carrying *all* of its `runs-on:` labels, and the
`self-hosted` label is conventional, not required. So the guard needs to know
which labels reach the persistent runner and which do not — and that is a fact
about the runner inventory, not something that can be read out of the workflow
text. It is therefore **declared here** and reviewed by a maintainer:

- Persistent self-hosted runner labels: `self-hosted`, `Windows`, `X64`, `godotgs`, `gpu`
- GitHub-hosted runner labels: `ubuntu-latest`

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

The persistent GPU runner is also the maintainer's workstation. That is an
accepted arrangement, but it means the machine carries developer and consumer
software whose settings silently change what every GPU gate measures. Two such
settings have already produced misleading results (#874, #875), so they are now
**controlled per job and verified at runtime** rather than left invisible.

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
unexpected while meaning the GPU jobs moved to a driver stack nothing else uses;
and it fails if the **control** run reports no layers at all, because a parser
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

### Which jobs

The GPU pool is derived, never listed here:
`tests/ci/test_preflight_runner_gpu_environment.py` takes every self-hosted job
in `.github/workflows/*.yml` carrying the `gpu` label — reusing the label-routing
classification from `tests/ci/test_release_builds_runner_trust.py` — and requires
each to export both the loader-filter pair *and* every per-layer opt-out at job
level, to run the preflight, and to run it
before the build. An empty derived set fails the guard rather than passing. At
the time of writing that set is `gpu-tests` and `gpu-harness`
(`baseline_qa.yml`), `module-validation` and `openworld-proof-evidence`
(`gaussian_production_gates.yml`), and `runtime-release-ci`
(`release_ci_runtime.yml`).

The self-hosted jobs *without* the `gpu` label — `guards`
(`gaussian_production_gates.yml`), `shader-validation`
(`gaussian_shader_validation.yml`), `build_windows` and
`build_windows_export_template` (`release_builds.yml`) — run on the same
physical machine but do not create a Vulkan device, so they are deliberately out
of scope. If one of them grows a GPU step it must also gain the `gpu` label,
which brings it into the derived set automatically.

## Scheduled Triggers

| Workflow | Schedule (UTC) | Behavior |
| --- | --- | --- |
| `baseline_qa.yml` | `30 3 * * *` | Runs in update mode and publishes the `gpu-harness-recaptured-baselines` artifact (recaptured PNGs + provenance); opens a recapture PR when `BASELINE_UPDATE_PAT` is provisioned. |
| `gaussian_production_gates.yml` | `30 3 * * 1` | Runs the non-blocking `openworld-proof-weekly` benchmark evidence surface. |
| `release_builds.yml` | `30 2 * * *` | Builds and publishes the nightly prerelease, then prunes older nightly releases and tags. |
| `release_ci_runtime.yml` | `0 6 * * *` | Runs the non-blocking `release-ci` runtime evidence lane on the self-hosted Windows GPU runner and uploads the runtime validation report. |

## Dependencies

- Python 3.11
- SCons/build toolchain for compiled lanes
- Self-hosted Windows runner attached to this repository with labels `self-hosted`, `Windows`, `X64`, `godotgs`
- Optional GPU evidence label `gpu` for the Windows evidence lane
- Vulkan-capable environment for render-path lanes
- `xvfb` for Linux non-headless rendering checks

## Archived Workflows

Disabled workflows are stored in `../archived-workflows/`.

- `benchmark.yml.disabled`
- `build-engine.yml.disabled`
- `gaussian_pipeline_validation.yml.disabled`
- `test_gaussian_splatting.yml.disabled`
- `test_phase4.yml.disabled`
