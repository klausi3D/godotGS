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
| Release Builds | `release_builds.yml` | Builds Linux and Windows editors for CI artifacts, nightly prereleases, and stable-tag publishes, plus the Linux and Windows `target=template_release` export templates. | Publishes Linux tarballs and Windows zips on the nightly schedule and on `v*` tag pushes. The `finite_math_guard` job blocks publication on every channel, and the `release_candidate_gate` job gates the stable/tag publish path (both-platform builds + `--mode candidate` validation, fail-closed); see below. The `build_linux_export_template` / `build_windows_export_template` jobs (#825) upload export templates as **artifacts only** — they are deliberately not wired into `release_candidate_gate` or `publish_release`; see [export templates](../../docs/development/export-templates.md). The `export_smoke_windows` job runs `tests/runtime/run_export_smoke.py` against the Windows template built by the same run — it exports the test project and launches the exported binary on the GPU runner, plus a negative control that requires an empty `custom_template/release` to be rejected for the missing-template reason specifically (a timeout, a crash or an unrelated error fails the control). It is blocking on the lanes it runs on (`push`/tag/schedule/dispatch), and it is the evidence that the template can actually ship a game. It is wired **into the publication dependency graph**, not beside it: `release_candidate_gate` and `publish_release` both list it under `needs:` (which is what makes publication wait for it) and both assert `result == 'success'` (which is what makes a failure block, since under `always()` a `needs:` entry alone gates nothing). A stable release always requires it; a nightly requires it whenever a Windows payload is actually published, and tolerates its absence only in the Windows-outage case where `build_windows` did not succeed and no Windows bytes ship. Kept honest by `tests/ci/test_release_publication_gating.py`, which evaluates both `if:` conditions over a truth table. |
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
- `gaussian_production_gates.yml` — `guards`, `module-validation` (form above).
- `gaussian_shader_validation.yml` — `shader-validation` (form above).
- `release_ci_runtime.yml` — `runtime-release-ci` (form above). This workflow has no `pull_request` trigger (schedule + `workflow_dispatch` only), so the guard is trivially satisfied; it is carried explicitly to keep the self-hosted job fail-closed if a `pull_request` trigger is ever added.
- `release_builds.yml` — self-hosted jobs `build_windows` (strict), `build_windows_export_template` (strict), `export_smoke_windows` (strict). The tag after each job is the guard form that job actually carries, and `tests/ci/test_release_builds_runner_trust.py` compares it against the workflow **per job**, so this line cannot go on claiming a form one of them has stopped using. **strict** = `if: github.event_name != 'pull_request'`, which skips **all** pull requests (fork *and* same-repo); **standard** = the repository-standard fork guard in the code block above, under which trusted same-repo PRs still run. All three Windows release lanes therefore run on `push`/tag/schedule/dispatch only. `export_smoke_windows` is the only one of the three that additionally carries the `gpu` label: it exports the test project against the template built by the same run and then *launches* the exported binary, and `export_smoke_probe.gd` requires a live RenderingDevice and a real window read-back, so a non-GPU runner could not produce the evidence the job exists for. The deviation is *narrower* than the standard form, never wider — it cannot admit fork code — but it does cost pull-request coverage, and that cost is accepted deliberately rather than overlooked: Windows-only packaging steps (PowerShell staging, zip, checksum) are first exercised after the branch reaches `master`, or on the nightly/dispatch run. Two things bound the exposure. The Windows-specific *naming* logic — the part that actually broke (#825, the `.console.exe` wrapper name) — lives in `tests/ci/resolve_export_template.py` and is unit-covered on every PR by `tests/ci/test_resolve_export_template.py`; and `build_linux_export_template` is GitHub-hosted, runs on pull requests, and drives the same resolver and the same package/checksum/upload shape. Moving these jobs to the standard guard would place a multi-hour template build on the single shared self-hosted runner ahead of the GPU gates on every same-repo PR, so it is a maintainer trade-off rather than a default. Kept in sync with the workflow by `tests/ci/test_release_builds_runner_trust.py`, which derives the self-hosted job set from `release_builds.yml` — by label routing, so a job that reaches the persistent runner through its custom labels alone (`runs-on: [Windows, X64, godotgs]`, no `self-hosted` label) is caught too — and fails if a job here is undocumented, documented but nonexistent, carrying neither accepted guard form, or carrying a different form than the tag above claims.

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
