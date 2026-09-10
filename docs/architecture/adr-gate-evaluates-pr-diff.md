# ADR: The required gate evaluates the PR's own diff; contract source deferred (#887)

- **Status:** Accepted (owner decision, 2026-08-14). Implemented in **#886**.
  **Written after implementation and disclosed as post-hoc** — the decision itself
  predates the code and was recorded in the Phase-1 remediation log, but this document
  was not filed before the diff, which is what R3 asks for. Recorded as a process
  deviation rather than backdated.
- **Risk class:** this document is R0 (`docs/**`). The change it records is **R3**
  (`.github/workflows/**`), for which `.agentic/policy.json` sets `adr_required = true`
  — so the ADR is required. Note that nothing in CI *enforces* that requirement today;
  it is checked by `check_pr_contract.py`, which grades no real PR (§5).
- **Findings:** `GS-AUDIT-TEST-001` (#887, legs b and c), `GS-AUDIT-TEST-002` (#888),
  `GS-AUDIT-DOC-003` (#889). Audit snapshot `55bd3953475`.
- **Verified against:** the shipped diff of #886 at `f2a1559a773`, not against the
  implementation reports. §6 lists the four drafted claims that did not survive that
  check and what they were replaced with.
- **Precedent:** [`adr-advisory-lane-ledger.md`](adr-advisory-lane-ledger.md) — same
  shape of problem (a lane that reports without gating).

## 1. Context

At the audit snapshot, `agentic-pr-gate` was the only required status check on `master`,
with `enforce_admins: true`. What it actually checked was not the change under review:

- `validate_review.py` and `check_pr_contract.py` ran against `.agentic/templates/*`
  fixtures with a hardcoded `--paths modules/gaussian_splatting/logger/example.cpp`;
- the one PR-derived step, "Re-derive risk class from this PR's diff", **only printed** —
  `classify_change.py` exits 0 for every class — and carried
  `if: github.event_name == 'pull_request'`, so it did not run in the merge queue at all;
- `validate_repo_contract.py` ran **without** `--strict-hierarchy`, so the entire
  `AGENTS.md` hierarchy and `docs/governance/*` could be deleted with the gate green;
- `classify_paths([])` returned `ordering[0]` (R0) while `policy.json` declares
  `default_unclassified: "R3"`, and the unit test asserted the R0 value — the suite
  defended the fail-open.

So the gate certified fixtures. Closing that requires the gate to read *something* about
the PR, and the natural candidate — the PR's task contract — does not exist: no per-PR
contract source is committed anywhere, and the root `AGENTS.md` forbids committing local
task instances.

## 2. Options for a contract source

1. **Diff-only now; contract source decided later.** *(Chosen.)*
2. **PR-body contract block, fail-closed for R2/R3.** Rejected for now. The PR body is a
   mutable, unversioned oracle: `edited` is not a default `pull_request` activity type,
   so the block can be changed after the gate ran green. `merge_group` carries no PR body
   at all, which would create an exemption on the enforcing path — the last boundary
   before `master`. Revisitable with mitigations: trigger on `edited`, snapshot the block
   into the check output, advisory-first rollout.
3. **Committed contract file under `.agentic/tasks/`.** Rejected for now. It requires
   amending the standing "do not commit ephemeral agent artifacts" rule, which is a
   governance change that must not ride an R3 gate rewrite, and its lifecycle (retention,
   stacked PRs, cleanup, who deletes a merged task) is undesigned.

## 3. Decision

The required gate derives everything it enforces from the immutable `base..head` diff.

### 3.1 What is enforced

- **The derivation, not the verdict.** Changed paths feed `classify_change.py`. The
  derived class is *not* a failure condition — R3 PRs are legitimate, and #886 is one.
  What fails the gate is the derivation being unsound: an unresolvable base ref, or a
  base whose policy cannot be read.
- **Fail-closed base resolution.** `resolve_diff_base()` resolves the merge base
  explicitly and exits non-zero when the ref or the shared history is missing. The
  previous code retried an unresolvable `base...HEAD` as a two-dot `git diff base`, which
  answers a different question and *succeeds* in cases where the three-dot form fails.
- **Fail-closed empty diff.** `classify_paths([])` returns
  `classification.default_unclassified`, read from the policy object rather than
  hardcoded. An empty diff is an absence of information, not evidence that nothing risky
  changed.
- **The policy comes from the base commit, not from the PR.** The gate materialises
  `git show "$GS_PR_BASE_SHA:.agentic/policy.json"` and passes it via `--policy`. Without
  this, a PR that edits the policy is graded by the rules it is changing: flipping the
  renderer rule R2→R0 is accepted by `validate_repo_contract.py --strict-hierarchy`
  (which only checks internal *consistency*) and drops renderer files to R1. Measured, on
  a renderer + `policy.json` diff with that flip: **R1** with the PR's policy, **R2** with
  the base policy, **R3** with both halves of the fix.
- **A policy edit floors at the top class.** `SELF_REFERENTIAL_PATHS` in
  `classify_change.py` forces any diff touching `.agentic/policy.json` to `ordering[-1]`
  (R3 under the shipped policy), independent of what either policy version says about it.
  Implemented in the classifier, not by editing `policy.json`'s own rules — the latter
  would be the same self-reference one level down.
- **`validate_repo_contract.py --strict-hierarchy`**, so the `AGENTS.md` hierarchy and
  `docs/governance/*` are required to exist.

Both halves of the policy fix are load-bearing. Forcing alone still lets `ordering` itself
be rewritten; the base policy alone lets the *next* PR inherit a weakened rule silently.

### 3.2 What is published but not enforced

The gate writes the derived class into `$GITHUB_STEP_SUMMARY` together with that class's
`evidence_requirements` **and** `deterministic_checks`, read out of the policy rather than
restated, so a human merging an R2 PR sees "Runtime/GPU harness or benchmark evidence
against the immutable base" on the required check itself. CI does **not** verify that any
of it was produced.

### 3.3 What is not consumed at all

Author self-declaration. The higher-of-the-two cross-check exists in
`check_pr_contract.py`, but that script runs in CI only as a self-test against
`.agentic/templates/task.json`. The limit is stated in
[`agentic-engineering.md`](../governance/agentic-engineering.md),
[`review-policy.md`](../governance/review-policy.md),
[`github-settings.md`](../governance/github-settings.md), `.github/workflows/README.md`,
and in the job summary itself — five places, because the two canonical governance docs
had already contradicted each other once on exactly this point.

## 4. How the gate's own wiring is guarded

`tests/agentic/test_agentic_pr_gate_workflow.py` pins the workflow, because a repaired
script whose wiring nobody checks is the "guard wired to nothing" shape from
[`evidence-integrity.md`](../governance/evidence-integrity.md). Every assertion has a
demonstrated RED mutation. The suite covers, at minimum: `--strict-hierarchy` removal;
deletion of the risk-class step; whole-workflow revert to the audit snapshot; a step-level
`if:`; a **job-level** `if:`; deletion of the step that runs this very suite; neutering the
command with an `echo`/`true`/`:` prefix; hardcoding `--base-ref`; removing `--policy`;
replacing the fail-closed base-policy guard with a swallowing fallback; removing
`merge_group` from `on:`; and every `||`-suffix form.

Three of those deserve naming, because each was a real false-GREEN that a green suite had
already passed:

- **Job-level `if:` is a bypass; the two shapes next to it are bricks.** A job "skipped by
  a conditional" reports Success — GitHub counts `success`, `skipped` and `neutral` as
  successful check statuses — so a job-level `if:` silently disables the whole gate.
  Renaming the job, or removing `merge_group` from `on:`, both *block* merging instead
  ("Expected — waiting for status to be reported"). Only the first is quiet, which is why
  it carries its own assertion.
- **`|| echo` was blessed, not missed.** The detector enumerated `|| true`, `|| :` and
  `|| exit 0`, and its own negative control asserted `python x.py || echo failed` was
  *safe*. It is not: `||` yields the right-hand side's status. It is now default-deny —
  **no `||` at all** in the gate's shell, with **no allowlist**, `|| exit 1` included.
  That last is a deliberate false-RED: the rule is auditable at a glance, whereas "no `||`
  whose right-hand side can succeed" requires classifying every command. Measured before
  choosing: the gate has no shell-level `||` today.
- **Deleting the step that runs the suite was invisible for three rounds.** It leaves every
  assertion in the file green while none of them ever executes on a PR again. Found by the
  independent verifier, not by the implementer or the review.

## 5. Consequences and declared limits

- **Scope and evidence contracts are not enforced against any PR.** `owned_paths`,
  `forbidden_paths` and `evidence_requirements` are never checked against real changed
  paths. A Phase-2 ADR decides the contract source; option 2 with an advisory-first ladder
  is the leading candidate.
- **The R2/R3 obligations are visible, not verified.** The merge decision stays with the
  human.
- **The guard is a text matcher and is evadable by construction.** It defends against
  *accidental* loss of enforcement, not an adversarial author — who has merge rights and
  is defended against by human review. `invocation_re`'s docstring enumerates the shapes:
  correctly accepted (`python3`, whitespace, step rename); safe false-REDs (`env python`,
  `python -m`, and reindentation, which is self-detecting because `yaml.safe_load` raises
  `ParserError`); and deliberately uncovered (`bash -c`, trailing `&`, `;`-chained no-ops,
  decoy comments). A related residual is declared on `shell_text()`: because GitHub
  expression spans are stripped before matching, a `||` *delivered into* the shell by an
  expression would be invisible. The gate's four expressions are static and none
  interpolates into a command.
- **The gate runs the PR's own classifier code.** `pull_request` checks out the PR, and
  `scripts/agentic/**` is R0, so a PR rewriting `classify_change.py` publishes an
  understated class. This is **pre-existing**, not introduced here, and it is
  *published-not-enforced* surface — it cannot bypass any other step. Running a base copy
  differentially is technically feasible (the same trick as the policy) but is not
  proportionate: the legitimate-divergence case is a PR that intentionally improves the
  classifier — every PR in this series — which needs an override, and an override on the
  only required check is a new fail-open of exactly the class this work closed. The cheap
  partial taken instead on #887 is adding `scripts/agentic/classify_change.py` to
  `SELF_REFERENTIAL_PATHS`: one tuple entry, no new machinery, can only ever *raise* a
  published class, and it is the consistent extension of a principle already accepted here
  — `policy.json` is the grader's data and `classify_change.py` is the grader itself, so
  forcing the data but not the code is asymmetric. `.agentic/schemas/*` and
  `.agentic/ownership.json` are deliberately **out** of that follow-up: they feed
  `check_pr_contract.py`, which grades no real PR until the contract source exists, so
  adding them buys nothing today.
- **A new way for the required check to go red.** A PR whose base SHA is absent from the
  checkout — force-pushed, deleted, garbage-collected — now fails the gate until rebased.
  That is the intended fail-closed behaviour; the alternative is the fail-open this ADR
  removes.
- **PyYAML is deliberately not a dependency of the required gate.** No workflow in this
  repository pip-installs it, `actions/setup-python@v5` provisions a bare tool-cache
  interpreter, and a module-scope `import yaml` in `tests/agentic/` would `ImportError` at
  unittest *discovery* and fail the only required check on every PR. When **#894 (T6)**
  makes PyYAML mandatory and fail-closed for the guard lane, port this guard to a real
  YAML parse — which closes the job-level construct class *structurally*, since a
  job-level `if:` becomes a different key rather than a different indent — and record in
  that PR which lane carries the dependency.
- **Branch protection remains hand-documented.** `github-settings.md` now carries the live
  `gh api` state, but nothing derives it. Settings-as-code, or a scheduled doc-vs-live
  drift check, is a Phase-2 item.

## 6. Drafted claims corrected against the shipped code

This ADR was drafted from the implementation reports. Four claims did not survive being
checked against `f2a1559a773`:

| Drafted claim | What ships | Why it matters |
| --- | --- | --- |
| "the derived risk class is **enforced**, not printed" | The *derivation* is enforced; the class is not a failure condition | The original wording would have a reader expect an R3 PR to be blocked |
| policy read from "the immutable **merge base**" | Read at the **base commit** `$GS_PR_BASE_SHA`. The *diff* uses the merge base | They differ once the base branch advances; conflating them misdescribes what is immutable |
| "the human … sees its **unmet** obligations" | Sees the obligations the class carries; CI cannot know which are unmet | Overclaims the gate's knowledge |
| "all `||` forms are default-deny with an explicit, reasoned **allowlist** for fail-preserving forms" | Default-deny with **no allowlist**; `|| exit 1` is rejected too, as a deliberate false-RED | An allowlist that does not exist would be the first thing a future editor "restored" |

## 7. Follow-ups

- **#887** — residual known-open items: the `JOB`-key-vs-`name:`-value gap (matching the
  job key, while the required context is the `name:` value, so renaming only `name:`
  bricks the repo with every test green); the untested stdout fallback in
  `classify_change.py`; the unpinned `--no-renames`; and the classifier-from-base thread
  above, whose accepted remedy is the `SELF_REFERENTIAL_PATHS` entry.
- **#889** — remaining stale branch-protection claims:
  `docs/reference/renderer-release-gates.md:41` and
  `docs/reference/renderer_release_gate_manifest.json:93-113`, plus
  `.github/workflows/AGENTS.md:27-32`, which is deferred to the repository owner because
  it carries uncommitted local edits.
- **#894 (T6)** — PyYAML as a mandatory gate dependency, unblocking the structural port
  described in §5.
- **Phase-2 contract-source ADR** — the decision this one defers.

## 8. What would change this decision

A committed per-PR contract source, or an accepted mutation of the "no local task
instances" rule, makes option 2 or 3 live and turns §3.2 from published into enforced. A
second required status check would also change the blast-radius calculus in §4: today a
broken edit to this one workflow blocks every merge in the repository, including its own
fix, which is why the changes it records were made in the smallest increments that could
still be proven.
