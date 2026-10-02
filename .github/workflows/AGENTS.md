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
  job name `agentic-pr-gate`, which `master` branch protection requires (see
  `docs/governance/github-settings.md`). If you rename the job, update branch
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
