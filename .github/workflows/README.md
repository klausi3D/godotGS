# GitHub Actions Workflows

This directory contains 6 active workflow files.

GitHub's Actions tab can also show historical workflow names from past runs, disabled files, or workflow files that are no longer present in this directory. This README tracks the workflow files currently checked into `.github/workflows/`.

## Active Workflows

| Workflow | File | Purpose | Notes |
| --- | --- | --- | --- |
| Baseline QA Automation | `baseline_qa.yml` | Runs baseline QA and optional compiled-module QA. | Builds the Linux editor once and reuses that artifact for push-only compiled QA. |
| Docs Pages (Versioned) | `docs_pages.yml` | Builds and deploys MkDocs docs with mike versioning to `gh-pages`. | Publishes `latest` from `master/main` and versioned docs from `v*` tags. |
| Gaussian Production Gates | `gaussian_production_gates.yml` | Enforces guard checks, pipeline smoke, runtime validation, the blocking streaming gate, and optional non-blocking benchmark evidence surfaces. | Owns the single Windows build for validation workflows. `streaming-gpu-ci` is the canonical blocking GPU-backed streaming runtime gate; `openworld-proof-dev` and `openworld-proof-weekly` are evidence-only benchmark surfaces. |
| Gaussian Shader Validation | `gaussian_shader_validation.yml` | Validates shader compile matrix and host/shader contract checks. | Focused shader CI gate. |
| Release Builds | `release_builds.yml` | Builds Linux and Windows editors for CI artifacts, nightly prereleases, and optional stable-tag publishes. | Publishes Linux tarballs and Windows zips on the nightly schedule and on `v*` tag pushes. |
| Agentic PR Gate | `agentic_pr_gate.yml` | Fork-safe, always-on gate: validates the agentic control plane, runs the agentic tests, the agentic/governance link check, and the GPU-free `--guard-only` lane. | GitHub-hosted (`ubuntu-latest`); runs on every PR and the merge queue. Required status check (job name): `agentic-pr-gate`. |

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
- `release_builds.yml` — `build_windows` uses the **stricter** `if: github.event_name != 'pull_request'`, which skips **all** pull requests (fork *and* same-repo); the Windows release lane runs on `push`/tag/schedule/dispatch only.

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
