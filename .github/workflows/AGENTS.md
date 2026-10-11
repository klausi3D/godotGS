# AGENTS.md — `.github/workflows`

Refines the root [`AGENTS.md`](../../AGENTS.md) for CI workflows. Workflow changes
that affect gates, runners, or the trust boundary are **risk class R3** and need
maintainer (CODEOWNER) review. Read `README.md` in this directory for the current
workflow inventory and the runner trust policy.

## Security rules (non-negotiable)

- **Least privilege.** Every workflow sets explicit `permissions:`; default to
  `contents: read` and add scopes only as needed.
- **No untrusted code on persistent self-hosted runners.** Self-hosted Windows/GPU
  jobs must carry the fork guard
  `github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository`
  so fork PRs are skipped. This is the required standard; the agentic-foundation
  series brings all self-hosted workflows under it, so verify a given job actually
  carries the guard rather than assuming universal coverage. Fork PRs get their
  blocking signal from the GitHub-hosted `agentic-pr-gate` check.
- **Never use `pull_request_target` to check out fork code with a privileged
  token or secrets.** If you think you need it, you don't — stop and ask a
  maintainer.
- **Fail closed.** When the trust level of an event is unclear, do not run the
  privileged/self-hosted path.

## Gate rules

- A required gate must **always report a terminal status** (no path filter that
  silently skips it into a missing-required-check state). The fork-safe required
  gate is `agentic_pr_gate.yml` (`ubuntu-latest`); its required status check is the
  job name `agentic-pr-gate`. The second required check is the `docs-build` job in
  `docs_pages.yml`. `master` branch protection requires both (see
  `docs/governance/github-settings.md`). If you rename either job, update branch
  protection in the same change, or every merge blocks on a check that never reports.
- Do not weaken or remove an existing guard, runtime gate, or release gate to make
  a PR pass.

## Process rules

- Keep `README.md` in sync: workflow count, triggers, schedules, and the runner
  trust policy. Validate YAML parses (`python tests/ci/validate_automation.py
  --contracts-only`).
- **Any change that relaxes the trust boundary or downgrades a gate must be
  documented** here, in `README.md`, and in `docs/governance/review-policy.md`,
  and explicitly approved by a human maintainer.

## Proposed universal evidence gate

The reviewed R3 redesign in `pr_gpu_evidence_verdict.yml` uses full immutable
commit/policy evidence for every supported same-repo/master PR, including docs,
and permits SUCCESS only for canonical single-parent commits. It removes the
former R0/R1 GPU exemption; classification remains informational. Mandatory
module tests, full actual GPU catalogue and runtime execution are unconditional and the runner fork guard remains
required. Existing advisory batches and completion-marker severity are preserved.
The default-code hosted publisher uses an exact master-only environment and a
dedicated App; its Actions token has no statuses-write permission. Activation is
pending owner approval and real hosted/native validation. Keep the acceptance and
availability costs synchronized in this directory's README, the review policy and
GitHub-settings checklist. A source change never installs protection by itself.

The canonical GPU report is a fifth fresh bound artifact; advisory `--test --gpu`
selection alone cannot qualify it. Fixed oracle/build/bootstrap/control inputs
are pinned. Only reviewed oracle/fixture data may use an exact owner-signed
bootstrap/migration binding transported through canonical environment JSON;
proposed privileged code never executes. Standard OpenSSH verifies the immutable
trusted-default owner public key and executable pin. Null pins disable all SUCCESS.
Environment writers and editable comments supply no approval authority. Full
new-policy proof precedes explicit owner P1→P2 required context replacement,
preserving strict and existing agentic/docs source checks. Fixed execution/key/tool
updates require separate maintenance scope. This acceptance/availability tradeoff
remains proposed until final human approval and actual activation evidence.
