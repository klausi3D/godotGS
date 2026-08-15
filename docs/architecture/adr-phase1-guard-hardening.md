# ADR: Phase-1 guard hardening — a closed cluster of remediations (#891–#910)

- **Status:** Proposed. **Filed before implementation**, which is what R3 asks for and what
  [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md) could not claim. No code in
  this cluster has been written. Nothing here is a report of work already done.
- **Risk class:** this document is R0 (`docs/**`). The changes it covers classify R1–R3; §1
  records each member's class as **measured** by `scripts/agentic/classify_change.py` at
  `adcd6916dbd`, not as planned.
- **Scope of the discharge:** `.agentic/policy.json` requires "Design record (ADR or
  design-change issue) before implementation" for R3. This ADR discharges that requirement
  **for exactly the tasks in §1 and for nothing else.** §1's closure rule is not decoration.
- **Findings:** `GS-AUDIT-TEST-004` … `TEST-013`, `GS-AUDIT-BUILD-001`, `GS-AUDIT-SYS-011`,
  `GS-AUDIT-GPU-017`, `GS-AUDIT-DOC-001`, `GS-AUDIT-DOC-002`. Audit snapshot `55bd3953475`.
- **Verified against:** `origin/master` at `adcd6916dbd`. Every count, class and file
  reference in §§1, 4–8 was re-derived from that tree; the derivations are named inline so a
  reviewer can re-run them rather than trust them.
- **Landing:** its own R0 PR (`docs/**` only), merged **before** any implementation PR in §1
  opens. The PR also adds the entry to [`index.md`](index.md), whose ADR list is the only
  hand-maintained registry and has drifted before.
- **Precedent:** [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md) (the R3
  disposition this ADR templates), [`adr-advisory-lane-ledger.md`](adr-advisory-lane-ledger.md)
  (measure-then-arm), [`adr-test-quarantine-manifest.md`](adr-test-quarantine-manifest.md)
  (declared, owned, expiring exclusions).

**Programme principle.**

> **Class follows the right design; design never follows class.** Scoping a fix down to dodge
> R3 is the same shape as weakening a gate to make it pass.

This governs every member of §1, and it is stated here rather than buried in a decision record
because it is the rule that produced two of the five decisions in §11. Where a member's class
was left open, the open thing was always the *design*; the class was then read off whatever
design turned out to be correct. The inverse move — picking a cheaper scope because it grades
R1, and calling that a scoping decision — is the failure this ADR's own subject matter exists
to catch, one level up. §11 Q1 (#895's scope) and §11 Q5 (T10's reach) are the two places the
principle was tested; both were settled by it, and Q1 was settled *against* the cheaper class.

## 1. Membership — this ADR covers exactly these tasks

Of the fifteen remaining Phase-1 sub-tasks, **eight measure R3 unconditionally** (#891, #892,
#893, #894, #896, #897, #898, #904); **#895 measures R3 on the scope the maintainer chose**
(§11 Q1); **#903 measures R3 once its design is written down in full** (§11 Q5, §6.6); and one
(#901) is deliberately floored at R3 by §7. **Eleven of the fifteen are R3.**
Read literally, each would need its own design record. They are one programme against one
defect shape — a guard that reports green while observing nothing — so they get one record.
The price of that economy is that the list is closed.

| Task | Issue(s) | Finding | Class (measured at `adcd6916dbd`) | Design |
| --- | --- | --- | --- | --- |
| T3 runtime completion marker | #891 | `TEST-007` | **R3** | §4 |
| T4 GPU retag + quarantine split | #892; children #906, #907, #908, #909, #910 | `TEST-006` | **R3** | §5 |
| T5 release waiver expiry | #893 | `TEST-005` | **R3** | §9 |
| T6 `validate_automation` wiring | #894 | `TEST-004` | **R3** | §9 |
| T7a synthetic-fixture floor | #895 | `TEST-009` | **R3** — full scope, enforcement at the consumer (§11 Q1) | §9 |
| T7b orphan runtime probes | #896 | `TEST-010` | **R3** | §9 |
| T7c GPU-evidence path filter | #897 | `TEST-011` | **R3** | §8 |
| T7d StringName orphan guard | #898 | `TEST-012` | **R3** (the task plan carried R1 — see note) | §9 |
| T8a metric provenance | #899 | `SYS-011` | **R1** alone; **R2** if the fix reaches `renderer/` | §9 |
| T8b frame-completion tracker | #900 | `GPU-017` | **R2** | §9 |
| T9a fork-delta truth + drift guard | #901 | `DOC-001` | **R1 → R3 by design** | §7 |
| T9b `nodes/README.md` truth | #902 | `DOC-002` | **R1** | §9 |
| T10 production-defaults pixel coverage + override-diff guard | #903 | `TEST-008` | **R3** — the class followed the design (§11 Q5). `TEST-008`'s second remediation clause is a new `tests/ci/check_*.py`, which self-registers per §7 and floors the PR at R3 (§6.6, measured). Independent of whether it reaches `baseline_qa.yml` | §6 |
| T11a stale fork claim | #904 | `TEST-013` | **R3** | §9 |
| T11b dead shaders | #905 | `BUILD-001` | **R2** | §9 |

Deliberately **not** members, and not covered: **T1** (#887, #888, #889), which landed as
**#886** with its own ADR; and **T2** (#890), in flight as **PR #911** with its own
disposition. Both are cited here (§3, §10) but neither draws on this record.

**Note on #898.** `GS-AUDIT-TEST-012`'s defect is at `tests/ci/run_module_tests.py:2936-2987`,
and that exact path is one of the five entries in policy's "CI deterministic-check /
release-gate machinery" R3 rule. A diff that fixes it is R3 by path, regardless of how small
the edit is. PR #911 measured the same escalation from the other side and declined the edit
rather than accept the class; here the class is accepted. See §7.1.

**Closure rule.** Any R3 change **not** in the table above needs its own design record. This
document may not be cited as the design record for work it does not name. Adding a row is an
**amendment to this ADR** — a revision proposed, reviewed and approved as a document change —
never an implementation detail decided inside the PR that wants the coverage. An open-ended
cluster ADR becomes the artifact future R3 PRs point at to skip their own; a closed list
cannot be stretched.

## 2. Programme decisions that apply to every member

These are stated once, here, and are binding on every PR in §1.

1. **Fail-closed over fail-open.** Every member is an instance of
   [`evidence-integrity.md`](../governance/evidence-integrity.md) required practice 1: a
   guard that cannot run must report a state that is not `pass`, and a consumer of required
   evidence must fail closed on it.

   **And its limit, which this cluster hit in §6.6: fail-closed is not a licence to build a
   guard that cannot pass.** A rule that reds on a condition the tree permanently satisfies is a
   **vacuous FAIL** — the mirror of the vacuous pass, and not the safe direction, because a gate
   that can never go green is deleted, suppressed or "temporarily" narrowed by whoever meets it
   next, and carries no information in the meantime. Both are the same underlying error: a
   guard whose output does not vary with the thing it guards. **Every guard must have a
   reachable RED and a reachable GREEN, and §2.4's acceptance evidence must show both.**
2. **A positive completion marker beats inferring success from absence.** Where a member has
   the choice between "no bad signal seen" and "a good signal produced", it takes the latter.
   §4 is the largest instance; the principle is not confined to it.
3. **Declared limits live in the guard's own docstring.** Not in a PR body, not in an issue
   comment. A limit a future reader cannot find while reading the guard is a limit that will
   be rediscovered as a defect.
4. **Acceptance is mutation-proof, in two directions.** Every repaired guard must be shown
   RED under **(a)** the defect it guards, and **(b)** its own wiring deleted — the step
   removed, the call site dropped, the function stubbed to a passing return. A fix whose test
   cannot fail is rejected, and (b) is not optional: in #886 the deletion of the step that
   *ran* the new suite went unnoticed for three rounds while every assertion in the file
   stayed green.
5. **Baselines, quarantine manifests and fingerprints change only through their documented
   flows.** And a corollary that bites T4 specifically: **a retag is a rename.** It moves
   name-keyed digests while counts stay identical, so an unchanged count is not evidence that
   nothing moved. Where a member changes case names or tags, the acceptance evidence is the
   derived membership set before and after, never the count.
6. **No prescription without a read.** Every sentence in a design record that says what a named
   file does, or must be changed to do, **cites the `file:line` it was read at**. A prescription
   written from memory of how a consumer *probably* behaves is not a design decision; it is a
   guess that a reviewer has to re-derive. Eight review rounds produced eighteen findings against
   this document and **twelve were the same defect**: an instruction that would have failed the
   very consumer it was written to satisfy, because nobody had opened it. A fifth manifest
   declaration the count check rejects and a #908 declaration the stale check rejects (§5.4); a
   soak trigger no profile selection can satisfy, evidence read from a report already
   overwritten, and an advisory status the exit expression counts as a failure (§4.6, §4.6.1); a
   defaults source that cannot resolve 14 of the 32 keys it must compare (§6.6); an expected-fail
   design that stops at the GDScript boundary while three Python mechanisms reject it (§6.3); its
   own replacement, which produced a representation no scene could both execute from and be
   inventoried under (§6.3 again, round 5); a derivation whose changed-path input drops one side
   of every rename (§8.4.1); the same input silently truncated at 300 files by a documented cap
   nobody re-read (§8.4.1 again, round 7); and a fail-closed rule with no green route, because
   the manifest deliberately inventories unregistered keys (§6.6); and a soak whose union
   property could never reach the two C++ scenarios that step 2 nevertheless arms (§4.6.2). The
   other six were genuine design gaps — a missing failure signature, a base-versus-head policy
   read, an under-specified membership check, the self-certification hole in §8.4, a completion
   marker never bound to the scenario that emitted it (§4.1), and an allowlist offered as the
   repair for a state it cannot reach (§4.6) — the normal cost of design review. The twelve were
   not. **Not one was a wrong judgement call; every one was an unread consumer.** Treat an
   uncited claim about a consumer's behaviour as unverified whatever its confidence.

   **How corrections themselves fared, tracked in three categories rather than two**, because
   the distinction changes what a reviewer should look for. Of the findings that landed on text
   an earlier round had already written: **three were fresh defects introduced by a correction**
   (§6.3's representation, and two others); **two were corrections that were incomplete rather
   than wrong** (§8.4.1's rename contract, missing the truncation precondition on the same
   input; §4.6's soak, which fixed satisfiability and left the C++ scope unstated); and the rest
   were **by design** — an existing rule deliberately extended to a new site, such as §10.1's
   symmetric enumeration guard. Only the first category is a regression. The second is the more
   instructive one: a correction that closes the named hole and leaves a sibling is the shape
   that survives review, because the section *looks* freshly examined.

   **One instance is worth naming for how it hid.** §4.6's union obligation said it made
   per-profile counting *"add up to a statement about the whole registry"* — correct number,
   wrong noun, since 11 `GDS_TESTS` is not the 13-scenario registry. That single word made the
   two C++ scenarios invisible to four subsequent rounds of review *of that same paragraph*,
   including two that rewrote it. A wrong noun in a claim about scope is not a wording problem;
   it is a specification that reviewers then verify against the wrong set.

   **The rule binds every layer, and did.** The `GLOBAL_DEF`-only defaults source (§6.6) came
   from the **maintainer's** §6.5 requirement, written without reading the registration
   mechanisms — committed, as it happens, while formulating this very rule. It was relayed
   unverified by the orchestrator, caught by Codex, and confirmed against the source by the
   maintainer. That chain is the point: the correction system prices claims by evidence, not by
   who made them, and it worked at every layer including the top one. A rule that exempted the
   lead would have shipped this defect into T10.

   **Stopping rule.** Rounds continue while findings are **real and land on prescriptions an
   implementer will consume**; they stop when a round **returns clean or degrades to polish**.
   The arithmetic justifies the cost: this ADR is the design record for eight R3 implementations,
   and a wrong contract here is rediscovered in the most expensive place available — inside an
   R3 PR, after the code exists, by a reviewer who must then relitigate the design. Five rounds
   of document review is cheap against one of those.
7. **Mechanism may be delegated; the invariant and the verified traps may not.** Where this
   record has been wrong at mechanism level more than once on the same subject, it states the
   **invariant** the mechanism must satisfy, records **every trap it verified with the
   `file:line`**, rules out any mechanism it has positively excluded, and delegates the
   remaining choice to the implementation PR — which owes a written record of the mechanism it
   chose and evidence that it clears the stated traps. §4.6.1/§4.7 and §6.3/§6.3.1 are the two
   places this applies; §8.4 is deliberately **not** one of them.

   **This is not scope retreat, and it should not be read as one.** Nothing decided is being
   withdrawn: the ladder, the soak's two obligations, the expected-fail semantics, the pinned
   signature, "do not weaken `validate_baseline_candidate`", and every measured number all stay,
   and §8.4 *adds* an invariant in the same revision. What moves is the last mile — a status
   string, a field name, a bucket layout — and it moves to where a verifier with a running
   harness settles it in minutes, rather than being guessed here and corrected over rounds. A
   design record that keeps guessing mechanism spends its credibility on the part it is worst
   placed to know, and every such guess so far has had to be withdrawn. **The test of the
   delegation is the obligation:** if an implementer could satisfy it without producing evidence
   a reviewer can check, the delegation failed and the section is under-specified, not
   appropriately scoped.
8. **One waiver idiom for the programme, reused deliberately.** Every declared exception this
   cluster introduces takes the quarantine-manifest shape — a keyed subject plus `reason`,
   `issue_url`, `owner`, `expires_utc`, with membership pinned and staleness failing — and it is
   now used in **three** new places (§6.3's expected-fail representation, §6.6's override
   waivers, §8.3's evidence exclusions) alongside the two existing precedents they copy
   (`quarantine_manifest.json`'s `unlaned_tests`, `renderer_release_gate_manifest.json`'s
   `deferred_requires_gpu_waivers`). **That consistency is a decision, not a coincidence.** A
   reviewer who has read one of these lists can audit any of them; an author cannot shop for the
   most permissive shape; and a defect found in one — as the membership-pinning gap was in §8.3
   — is fixable in all of them at once. A member proposing a differently-shaped exception list
   states why in its PR, and "it was easier here" is not a reason.
9. **Renames are this programme's demonstrated blind spot. Every path- or name-matching input
   states how it handles both sides of one.** Not a caution — a measured pattern, four
   independent instances, each found by a different route:

   - **§2.5** — a retag is a rename: it moves name-keyed digests while counts stay identical, so
     an unchanged count is not evidence that nothing moved.
   - **PR #911** — a whole-file rename of a baselined test file produces a false-RED, carried on
     **#890** (§10.2). Safe direction, but the same blind spot pointing the other way.
   - **§8.3** — the exclusion list pins *membership*, not a count, precisely because a rename
     holds the count while moving the set.
   - **§8.4.1** — the changed-path input drops `previous_filename`, so an R2 file renamed out of
     the R2 globs silently loses its evidence lane.

   The instances are not variations on one mistake; they are four different mechanisms, and each
   was found separately after the previous one was fixed. So the obligation is stated once and
   applies to every member: **a path- or name-matching input that does not say what a rename does
   to it is under-specified**, and the reviewer's question is the same every time — *what does
   this see when the thing it matches on is renamed, and does it fail loudly or quietly?*
   Quietly is the answer three of the four gave.

## 3. The R3 obligations disposition: cited, then instantiated

R3's evidence requirements include "Two independent reviews and CODEOWNER + human approval."
With a sole collaborator, two of those three are not achievable in form. **#886** established
the disposition; this ADR provides it as a **template**, not as a blanket.

The template, from #886:

> **R3 obligations disposition (owner).**
> The independent-review requirement is met in substance by two machine reviewers: *[an
> adversarial verifier with independently reproduced mutation evidence]* and *[a Codex review
> pass]*, which found *[N]* real defects, *[all fixed here with RED proofs / dispositioned as
> follows]*.
> The second-human-reviewer and CODEOWNER formalities are waived: structurally impossible
> with a sole collaborator.
> The ADR requirement is satisfied by `docs/architecture/adr-phase1-guard-hardening.md` §*[N]*,
> filed before implementation.
> The `streaming-gpu-ci` deterministic check *[was run / is waived: no runtime-reachable code
> in this diff]*.
> Known-open limits are enumerated in the guard docstring and tracked on *[#NNN]*.

**Every PR carries the pointer plus its own facts.** The bracketed slots are the point of the
template. A disposition that says *"Per #886 precedent"* and stops is a rubber stamp, and
rubber-stamping a waiver is the normalisation dynamic this programme exists to end. A
reviewer must reject a disposition that does not state, for that PR:

- **which** machine reviewers ran, and **how many rounds**;
- **how many findings** each produced, and the disposition of each — fixed with a RED proof,
  deferred with an issue, or rejected with a reason;
- whether the **`streaming-gpu-ci`** check genuinely did not apply to **this** diff. The
  waiver is available only where the diff contains no runtime-reachable code. Most members of
  §1 do not qualify: T3 edits the runtime harness itself, T4 retags C++ cases the harness
  executes, T10 adds QA scenes. For those, "waived" is wrong and "not run" is not "passed".

A correctly instantiated paragraph, for illustration (**the numbers below are placeholders** —
they are what a real PR replaces, not facts about any PR):

> **R3 obligations disposition (owner).** Independent review is met in substance by two
> machine reviewers, per the #886 precedent. The adversarial verifier ran 3 rounds and
> reproduced the mutation evidence independently; it found 2 real defects — the retag left
> `Stage-B instance depth culling toggles` outside the new `[Streaming]` filter because its
> name carries no `Streaming` token, and the manifest count stayed at 59 across the retag so
> the count check could not see the move — both fixed here, each with the RED transcript in
> the verification table. Codex ran 2 passes / 4 threads: 1 real defect (fixed, RED proof), 3
> dispositioned on the thread as no-change with reasons. Second-human-reviewer and CODEOWNER
> formalities are waived: structurally impossible with a sole collaborator. The ADR
> requirement is satisfied by `docs/architecture/adr-phase1-guard-hardening.md` §5, filed
> before implementation. **`streaming-gpu-ci` was run, not waived** — this diff retags C++
> cases the GPU harness executes, so the runtime-unreachability argument does not apply;
> result and runner recorded in the verification table above. Known-open limits are in
> `run_gpu_harness.py`'s docstring and tracked on #892.

## 4. T3 (#891) — the runtime completion-marker contract

`_classify_result` (`tests/runtime/run_runtime_validation.py:337-378`) treats `passed` as the
**fall-through**: exit 0, no `[RUNTIME_FAIL]`, no `[RUNTIME_SKIP]` ⇒ pass. A scenario that
prints nothing and exits 0 is indistinguishable from one that asserted everything. The argv
flags in the workflows defend only the *skip-marker* path; they do not touch this one.

### 4.1 The marker

**Decision.** A fourth marker, alongside `SKIP_MARKER` / `FAIL_MARKER` / `METRICS_MARKER` at
`run_runtime_validation.py:33-35`:

```
[RUNTIME_PASS] {"scenario": "<registry name>", "assertions": <int>}
```

Same grammar as `[RUNTIME_METRICS]` — a marker token followed by one JSON object on one line —
so the parser reuses the existing extraction shape rather than inventing a second one. Both
fields are required; a malformed or absent payload is a failure, not a missing-optional.

`_classify_result` gains one branch, evaluated after the existing failure and skip branches:
exit 0, no failure markers, no skip markers, **and no `[RUNTIME_PASS]`** ⇒ the scenario is
**not** `passed`. The fall-through to `passed` is deleted; `passed` becomes reachable only from
a marker.

**And the marker must be bound to the scenario it is classifying.** Requiring the `scenario`
field while never comparing it to anything makes it decoration: a scenario whose emitter was
copied from another — the *expected* way this spreads, since §4.2 puts emission in 15 separate
`.gd` files and §4.2's shared library is adopted "as they are touched" — emits a syntactically
valid marker naming a different registry member, and the branch above passes it. The harness
would then accept *some* scenario's completion as proof of *this* scenario's completion. That is
the same self-vouching shape §4.2 rejects when it forbids the harness from synthesising the
marker it checks; it simply moves the unchecked claim from the harness into the payload.

**Decision: `[RUNTIME_PASS]`'s `scenario` must equal the registry name of the scenario being
classified (`result.name`), and a mismatch is a failure — not a warning, and not silently
tolerated as a stray marker.** The field then does the one job it exists for: it makes the
marker non-transferable, so a marker proves completion of the scenario it was emitted by.

**This is load-bearing for §4.6's soak, not a tidiness point.** The soak's first obligation
counts `[RUNTIME_PASS]` per scenario *that the profile selects*. An unbound marker means one
scenario can vouch for another, so a profile could satisfy per-profile completeness while a
scenario it selected never completed — and the union-coverage obligation, which is what makes
per-profile counting add up to a statement about the whole registry, would be counting
signatures rather than completions. The arming evidence for the fail-closed flip would be
exactly as strong as the assumption nobody copy-pasted an emitter.

**Which status that branch assigns depends on the ladder step, and is the whole of §4.6's
advisory phase.** In step 2 it is `"failed"` with reason `no completion marker`. In step 1 it
must be a status that is *recorded and printed without failing the run* — and, as §4.6.1 works
through, the runner has no such status today, so step 1's PR creates one. Writing "`status =
'failed'`" here unconditionally, as an earlier revision of this section did, silently deletes
the advisory step the whole ladder is built on.

### 4.2 Who emits it

**Decision: the scenario emits it; the harness only verifies it.** A harness that synthesises
the marker it checks is the tautology this programme exists to remove.

There is no shared GDScript test library today — 15 of the 16 `.gd` files under
`tests/runtime/` declare their own `const METRICS_MARKER := "[RUNTIME_METRICS]"` and friends.
Copying a fourth constant into 15 files repeats what the existing three already cost.
**Decision:** add one
shared `tests/runtime/gs_runtime_report.gd` that owns the marker constants and the assertion
counter, and port scenarios onto it as they are touched.

### 4.3 A scenario that legitimately asserts nothing

**Decision.** `assertions: 0` is a failure **unless** the scenario also emits a
`no_assertions_reason` string *and* its registry name appears in an explicit, expiring
allowlist in `runtime_scenarios.json`. Same posture as the quarantine manifest: an untracked
exemption is not available; a tracked one is, with an owner, an issue and an expiry.

This is not new doctrine in the repo. The GPU harness already treats "ran, asserted nothing"
as distinct from "passed" (`case_assert_audit_ok`, `zero_assertion_cases`, #695/#696), and
[`adr-advisory-lane-ledger.md`](adr-advisory-lane-ledger.md) §4a defines `zero_coverage` for
the same reason. T3 gives the runtime lane the concept it is missing.

### 4.4 `fail_on_skip` defaults moving into profiles

**Measured: this changes no CI lane, and one local path.**

Precedence is `--fail-on-skip` > `--allow-skips` > profile `fail_on_skip` > implicit
`resolved_gd_mode != "headless"` (`run_runtime_validation.py:1145-1153`). All three CI
invocations pass `--fail-on-skip` explicitly and therefore never reach the profile key at all
(`gaussian_production_gates.yml:344,356`; `release_ci_runtime.yml:117`). Under each profile's
own declared `gd_mode`, the effective value today is already `true` for all six profiles:
`headless-ci` sets `true` explicitly against a headless default of `false`; the other five
are `non-headless` or `windows-vulkan` and inherit `true`.

So making the defaults explicit is behaviour-preserving where it matters. What it removes is
the *mode-coupled implicit default*: today a local `--gd-mode headless` run of `release-ci`,
`stress-only` or `smoke` silently flips `fail_on_skip` to `false`, because those three carry
no explicit key. After the change the profile's declared value holds regardless of the mode
override. That is a behaviour change on the local path only, and it is the intended direction —
a flag that silently loosens a gate when you change an unrelated setting is the same
absence-as-success shape one level up.

`TEST-007`'s evidence line describes `stress-only`/`smoke` as setting no `fail_on_skip`, which
is true of the key and **not** true of the effective value under their declared modes. The
finding's direction stands; that sub-claim is narrowed here rather than repeated.

### 4.5 The renderer-proof probe fails closed when the probe is absent

`_rendered_content_ok()` (`test_canonical_node_asset_render.gd:343-347`) returns `true` when
`rendered_content_probe_available` is false — the repo's only runtime renderer-proof emitter
passes when it could not look. **Decision:** make it a conjunction — probe available **and**
content seen. A renderer without `has_rendered_content()` (`:180`) then fails the proof.

`_build_renderer_proof_summary(required=True)` already fails when *no* proof metrics were
emitted at all (`:795-797`); the uncovered case is precisely *present-but-unavailable*, which
is the one this closes.

### 4.6 The real decision: advisory first, then blocking

**Decision: the repo's established advisory-then-blocking ladder, not an immediate
fail-closed flip.**

An immediate flip turns every unported scenario RED in one PR, and the pressure that follows
is to relax the parser — which recreates the defect with a fresh justification. The ladder:

- **Step 1 (this cluster).** The parser learns the marker. A scenario without it is recorded
  as `no_completion_marker` in `runtime_validation_report.json` and printed, but does not
  fail. All 11 registered GDScript scenarios are ported in the same PR, and their measured
  per-scenario assertion counts are recorded. **Porting all 11 is not the same as observing all
  11 in one run**, and step 2's trigger turns on that distinction: every registered scenario
  gets the marker here, but no single profile *selects* all 11, so no single run can witness
  them. Step 1's obligation is over the registry; step 2's is over what each profile runs.
  **Making step 1 actually advisory takes two coupled changes to the runner — §4.6.1.**
- **Step 2 (gated on step 1's numbers, and it stays in this cluster).** The parser flips to
  fail-closed, and the assertion counts measured in step 1 are pinned as a shrink-only floor.
  Arming against measured values is the [advisory-lane-ledger](adr-advisory-lane-ledger.md)
  pattern; arming against a hoped-for value is how a ratchet ends up pinning zero.

  **The trigger is a soak, not a date and not a phase boundary** (settled: §11 Q4). Deferring
  the flip to "Phase 2" makes it someone's forgotten TODO, and deferred enforcement that
  nobody is holding is exactly the rot this audit catalogued — an advisory lane with no
  arming condition is an advisory lane forever. So the flip has a condition it can actually
  meet, in this cluster:

  - **Proposed soak, as two distinct obligations.** They are separate on purpose; collapsing
    them into one is what made the first draft of this paragraph unsatisfiable (see the
    correction below).
    1. **Per-profile completeness.** 5 consecutive runs of each CI-invoked profile, with
       `[RUNTIME_PASS]` present for every scenario **that profile selects** — not every
       registered scenario — and no `no_completion_marker` record in that profile's report for
       any of those runs.
    2. **Union coverage — over the GDScript registry only, and that scope is the point.** The
       profiles in the soak set must, between them, select **all 11** `GDS_TESTS` scenarios,
       asserted separately as a property of `runtime_scenarios.json` at the moment the flip
       lands. What that makes obligation 1 add up to is a statement about **the 11 GDScript
       scenarios — not the registry**, which holds **13** (11 `GDS_TESTS` + 2 `CPP_TESTS`).

       **An earlier revision of this obligation said "the whole registry", and that sentence is
       what hid §4.6.2 for four rounds.** The number was right and the noun was wrong: 11 is the
       whole *GDScript* registry and 11 ≠ 13. Reading it as "the registry" made the two C++
       scenarios invisible to every subsequent review of this section, including the ones that
       rewrote it. The scope is now stated in the obligation itself rather than inferable from
       the constant beside it.

    The profile set is the three profiles CI actually invokes — **`headless-ci`** and
    **`streaming-gpu-ci`** (`gaussian_production_gates.yml:346,358`) and **`release-ci`**
    (`release_ci_runtime.yml:119`) — not all six declared profiles. A profile no lane invokes
    cannot soak, and counting it as soaked would be the same absence-as-evidence shape one
    level up.

    **Both halves measured at `adcd6916dbd`**, by reading `gd_tests` out of
    `tests/runtime/runtime_scenarios.json` and the registry out of `GDS_TESTS`
    (`run_runtime_validation.py:107-119`):

    | Profile | Scenarios selected | Covers all 11? |
    | --- | --- | --- |
    | `headless-ci` | 3 | no |
    | `streaming-gpu-ci` | 3 | no |
    | `release-ci` | 10 | no |
    | **union of the three** | **11** | **yes** |

    The union holds, but only just, and on one scenario: **`Streaming GPU Tier Budget
    Contract` is selected by `headless-ci` alone** and is the single scenario `release-ci`
    omits. Drop `headless-ci` from the set, or edit its `gd_tests`, and the union silently
    stops covering the registry while every remaining obligation still passes.

    **So obligation 2 is a guard, not a note.** The flip PR asserts the union property in code
    — derived from `runtime_scenarios.json` and `GDS_TESTS`, never from a hand-written list of
    scenario names, per §2 and `evidence-integrity.md` practice 5 — and **a future profile edit
    that breaks it must fail loudly.** A registered scenario that no soaked profile selects is
    a coverage hole, and the failure must name the orphaned scenario. The forbidden outcome is
    the quiet one: the soak keeps passing over a shrinking share of the registry, and the
    fail-closed flip is armed by evidence about scenarios nobody ran.

  - **Correction, recorded because the mistake is instructive.** The first version of this
    paragraph required `[RUNTIME_PASS]` from *every registered scenario in every run*. Measured
    against the table above, **no profile selects all 11**, so no run could ever qualify, step 2
    could never arm, and the missing-marker path would have stayed advisory forever. That is
    precisely the outcome §11 Q4 exists to prevent — an unsatisfiable trigger is a forgotten
    TODO with a schedule attached — and it is a vacuous gate in the strictest sense this repo
    means it: a condition that can never fire can never discriminate, so it would have read as
    a rigorous trigger while being no trigger at all. It is recorded here rather than quietly
    corrected because the tightening felt like *strengthening* the requirement at the time. The
    next author revising this trigger should check satisfiability against real profile
    selections before tightening it again.
  - **This N and this profile set are a proposal, not a measurement.** They are chosen before
    step 1 has produced a single run. The flip PR must confirm them against real runs and say
    so: if 5 runs is too few to have seen the flaky path, or a profile turns out never to emit,
    the flip PR revises the trigger *and states the revision* rather than quietly meeting the
    letter of this paragraph. One cost is already visible and the flip PR must not be surprised
    by it: **`release-ci` is a nightly evidence lane, not a PR gate**
    (`release_ci_runtime.yml`, `cron: "0 6 * * *"`, "intentionally NOT a required PR gate"), so
    5 consecutive `release-ci` runs is 5 nights of wall-clock. If that latency is the reason to
    change the trigger, the flip PR says so and proposes the alternative — it does not silently
    drop `release-ci` from the set.
  - **Precondition: the reports the soak reads must survive the run. Today one of them does
    not.** Verified at `adcd6916dbd`: neither invocation in `gaussian_production_gates.yml`
    passes `--report-path` (there is **no** `--report-path` anywhere under `.github/workflows/`),
    so both fall back to the same default — `tests/runtime/runtime_validation_report.json`
    (`run_runtime_validation.py:217-221`, default at `:219`) — inside the **same job and the
    same workspace**. The write is truncating (`report_path.open("w")`, `:1205`), so the
    `streaming-gpu-ci` run at
    `:351-360` **overwrites the `headless-ci` report** written at `:339-349`, and the single
    upload step at `:362-367` ships only the survivor.

    The consequence is exactly the error this cluster exists to remove: the soak would try to
    confirm that no `no_completion_marker` appeared in the **headless** reports by reading a
    file that no longer contains them, and would read that silence as a pass. Absence of a
    signal is never a passing signal — and here the absence is not even a gap in the evidence,
    it is evidence that was produced and then destroyed. Note that `headless-ci` is the only
    profile selecting `Streaming GPU Tier Budget Contract`, so the lost report is the one
    carrying the union's single load-bearing scenario.

    **Therefore: distinct `--report-path` values per invocation, and both artifacts preserved,
    are a precondition of the soak counting at all.** No qualifying run exists before this
    lands. This is **T3 implementation work on `.github/workflows/gaussian_production_gates.yml`**,
    and it does not move T3's class — measured, not assumed:

    ```
    $ python scripts/agentic/classify_change.py --paths \
          tests/runtime/run_runtime_validation.py tests/runtime/gs_runtime_report.gd \
          tests/runtime/runtime_scenarios.json
    risk_class: R3
    $ python scripts/agentic/classify_change.py --paths \
          tests/runtime/run_runtime_validation.py tests/runtime/gs_runtime_report.gd \
          tests/runtime/runtime_scenarios.json .github/workflows/gaussian_production_gates.yml
    risk_class: R3
      R3  .github/workflows/gaussian_production_gates.yml  (Release / security / CI workflow surface)
    ```

    T3 is R3 on `run_runtime_validation.py` alone; the workflow edit adds a second R3 path and
    changes nothing about the obligations. **This ADR does not make that edit** — it is an R0
    design record, and the workflow change belongs to T3's implementation PR. A later reader
    must not assume it is already done: check the two invocations for `--report-path` before
    counting a single soak run.

    `release-ci` is unaffected — it runs in its own workflow and job and uploads under its own
    artifact name (`release_ci_runtime.yml:124-129`), so nothing overwrites it.

  - **If the soak exposes marker gaps, that is a finding to fix, not a reason to stay advisory
    silently.** A scenario that intermittently omits the marker has an intermittent completion
    path, which is the defect `TEST-007` describes.

    **Two states that an earlier revision of this bullet conflated, and they have different
    repairs:**

    | Observed | What it means | Repair |
    | --- | --- | --- |
    | Marker emitted, `assertions: 0`, `no_assertions_reason` present | The scenario ran and legitimately asserted nothing | Tracked, expiring §4.3 allowlist entry — owner, issue, expiry |
    | **No marker at all** | The scenario is **unported**, or its completion path is intermittent | **Port it, or fix the path. Nothing else.** |

    **The §4.3 allowlist is not available for a missing marker, and the reason is structural
    rather than stylistic:** §4.3 grants the exemption only when *"the scenario also **emits** a
    `no_assertions_reason` string"*. A scenario that emits nothing has no marker to carry the
    reason, so the allowlist route is unreachable for it by construction — the earlier wording
    offered a door that does not open.

    It also must not be made to open. An allowlist entry for a missing marker would convert
    *"nobody ported this scenario"* into a **declared, owned, expiring — and therefore
    renewable — exemption**, which is how the advisory phase becomes permanent through the front
    door: each renewal individually justified, the ladder never climbed. That is precisely the
    outcome the soak exists to prevent, arriving with better paperwork than the failure it
    replaced. "Extend the soak until it goes green" is not an available response either, and
    neither is letting the advisory step run on with no one holding the trigger.

**Migration cost, stated honestly.** The registry holds **13** scenarios: 11 GDScript
(`GDS_TESTS`, `run_runtime_validation.py:107-119`) and 2 C++ (`CPP_TESTS`). Plus **4** orphan
probes (`GS-AUDIT-TEST-010` / #896) that are in no registry, no profile and no workflow.

- The 11 GDScript scenarios are ported by the T3 implementer, in step 1.
- The **2 C++ harnesses** must emit the marker too, and **nothing in CI will observe it**:
  `--skip-cpp` is universal across all three CI invocations
  (`gaussian_production_gates.yml:348`, `:359`; `release_ci_runtime.yml:121`), and
  `run_cpp_harnesses` (`:533`) hardcodes `fail_on_skip=False` (`:569`). **These ports are not
  bookkeeping** — an earlier revision called them that, and §4.6.2 withdraws it. They are the
  code step 2's fail-closed branch acts on the first time anyone runs the C++ scenarios:
  load-bearing, with no lane watching them, which is a more dangerous shape than bookkeeping
  rather than a less important one. Porting them is still not *evidence*, and the PR must not
  present it as evidence; §4.6.2 says what the arming evidence for these two actually is.
- The **4 orphans** are #896's business, not T3's. **Sequencing:** land #896 before step 2, so
  the fail-closed flip never has to reason about scenarios that may be deleted. If #896
  registers them, they are ported with it; if #896 deletes them, they never need the marker.

### 4.6.1 Step 1's advisory phase: the invariant, and two traps any mechanism must clear

**The runner has no non-failing way to record "ran, produced no completion marker".** Read at
`adcd6916dbd`:

- `TestResult.status` defaults to `"failed"` (`run_runtime_validation.py:57`).
- `summarise()` (`:1015`) counts `"failed": sum(1 for r in results if r.status == "failed")`
  (`:1019`).
- `main()` (`:1060`) ends `return 0 if summary["failed"] == 0 and summary["schema_valid"] and
  not renderer_proof_failed else 1` (`:1219`).

So a scenario classified `"failed"` for a missing marker **exits 1**. Step 1 says such a
scenario "is recorded … and printed, but does not fail"; §4.1's earlier wording said `status =
"failed"`. Those cannot both hold, and the loser is the advisory phase — the one the soak exists
to evaluate would never occur, because the first unported scenario reds the run.

**And the naive fix trades one exit-1 for another.** The summary schema pins the status
vocabulary: `allowed_statuses = {"passed", "failed", "skipped"}` (`:861`), enforced at
`:874-875` (*"status must be one of …"*), collected into `schema_errors` and reduced to
`summary["schema_valid"] = len(schema_errors) == 0` (`:1197`) — which is **a second term in the
same exit expression** at `:1219`. Inventing a status string without widening the vocabulary
turns a scenario failure into a schema failure. Same exit code, worse diagnosis.

**Invariant, and it is what this section decides:** step 1 must be able to record "ran, no
completion marker" in a way that is **visible in the report, counted in the summary, and does
not fail the run** — and whatever achieves that must satisfy **both** terms of `:1219`
simultaneously, not one at a time. Any mechanism meeting that is acceptable.

**Two traps any mechanism has to clear, verified above:** the `failed` count (`:1019`) feeding
`:1219`, and the schema-pinned status vocabulary (`:861`) feeding `schema_valid` (`:1197`) into
the *same* expression. A design that clears one and not the other exits 1 either way; only the
error message changes.

**One mechanism is ruled out rather than left open: reusing `"skipped"`.** It is already in
`allowed_statuses`, so it looks free, and it is wrong twice over. A skip means *the scenario did
not run*; a missing marker means *it ran and proved nothing* — the exact distinction §4.3 and
`TEST-007` exist to draw, and collapsing them re-creates the defect one column over. It also
collides with the `fail_on_skip` precedence chain (§4.4): every CI invocation passes
`--fail-on-skip` explicitly, so a scenario parked as `"skipped"` fails the run anyway.

**Everything else about the mechanism is T3's PR to decide** — the status string, the
`allowed_statuses` and report-schema edit, the summary field names, whether the advisory status
survives step 2 or is removed by it. This ADR has now specified this mechanism twice and been
wrong twice (§2.6), and the reason is structural rather than careless: the questions left are
settled in minutes by someone running the harness and in rounds by someone reading it. See §4.7
for what T3's PR owes in exchange.

### 4.6.2 The two C++ scenarios cannot be soaked, and step 2 must not pretend otherwise

**No CI lane observes the C++ emitters.** `--skip-cpp` is passed by every CI invocation of the
runner — `gaussian_production_gates.yml:348` and `:359`, `release_ci_runtime.yml:121` — and
`run_cpp_harnesses` classifies its own results with the check disarmed:
`_classify_result(result, fail_on_skip=False, allow_skip_tests=set())` (`:569`). So the soak,
which is defined over CI-invoked profiles, is **structurally incapable** of covering
`CPP_TESTS`. Not unlikely to cover them — incapable.

**But step 2 arms them anyway.** After the flip, any run that includes the C++ scenarios passes
them through the same `_classify_result` (`:337`; main path `:1006`) and fails closed on a
missing or malformed marker. So the flip would arm fail-closed behaviour for two scenarios whose
emitters no soak run has ever observed — **arming on evidence that cannot exist**, which is the
ratchet-pinned-to-a-hoped-for-value failure §4.6 names in its own text, committed by §4.6.

**Decision: step 2 arms both kinds, and the C++ pair's arming evidence is a recorded run of the
harnesses with `--skip-cpp` omitted, produced by the flip PR itself — not by the soak.**

The soak measures CI lanes; the C++ pair is in no CI lane, so asking the soak to cover it is
asking for a measurement of something unobserved. A single recorded run is weaker evidence than
five consecutive soak runs, and it is stated as weaker — but it is **real**, which the soak's
coverage of these two scenarios could never be. It is also achievable today: `run_cpp_harnesses`
exists (`:533-570`) and CI already builds the binaries the harnesses need, so the flip PR runs
them once, without `--skip-cpp`, and records the transcript showing both emit a well-formed
marker bound to their own registry name (§4.1).

**Step 2 is not weakened by this**, and that is the reason for choosing it over the alternative
of narrowing the flip to GDScript: narrowing would leave two registry members permanently
exempt from a fail-closed contract that applies to everything else, which is the structural
exemption §4.6's migration paragraph already refuses for these same two scenarios.

**If the flip PR cannot produce even one observed run**, then the C++ pair is deferred out of
step 2 — and deferral has the same price here as everywhere else in this programme: an **owner,
a tracking issue filed by the flip PR** (referenced with `refs`, never a closing keyword), an
expiry, and an explicit statement in the ADR and the PR that step 2's scope is GDScript-only.
**Silently arming them, or silently not arming them, are both unavailable.**

**And §4's description of these ports as "bookkeeping" is withdrawn.** The migration paragraph
above called porting the two C++ harnesses *"bookkeeping against a lane that does not run"*.
That was true about the *observation* and wrong about the *consequence*: the ports are what
step 2's fail-closed branch will act on the first time anyone runs the C++ scenarios, so they
are load-bearing code with no lane watching them — which is a more dangerous shape than
bookkeeping, not a less important one.

### 4.7 What T3's PR owes for the delegated mechanism

Per §2.7, delegation is paid for with a written record. T3's PR must state, in the guard's own
docstring or the PR body:

1. **The mechanism chosen** — the status value, where the vocabulary and report schema were
   widened, and which summary field carries the count.
2. **Evidence it clears both terms of `:1219` at once** — a run in which a scenario omits the
   marker, showing the scenario recorded and printed, `summary["failed"]` unchanged, and
   `schema_valid` still true. Both facts from one run, not two arguments.
3. **The step-2 disposition** — whether the advisory status is removed by the flip or retained
   for §4.3's allowlisted zero-assertion cases. Leaving a status in the schema that nothing can
   produce is its own dead-guard shape.

**Inadequate:** naming the status and stopping; asserting the exit code is unaffected without a
run that omits a marker; or a run that omits a marker but does not show `schema_valid`. Each of
those is the specific gap that produced §4.6.1's two corrections, so a reviewer should read this
list as three things to check rather than three things to skim.

## 5. T4 (#892, #906–#910) — batch grouping and the hollow batch

### 5.1 The derived partition

Re-derived at `adcd6916dbd` by importing `BATCHES` from `tests/ci/run_gpu_harness.py`,
enumerating every `TEST_CASE` under `modules/gaussian_splatting/tests/` and `tests/`, and
matching with `doctest_wildcmp` from `tests/ci/check_gpu_sorting_order_coverage.py` — doctest
semantics (brackets literal, case-insensitive), each batch's `excludes` subtracted. **Not
hand-picked**, and re-derivable by any reviewer with the same three inputs.

- **155** `[RequiresGPU]` cases written; **8** linker-dropped (`test_gpu_sorting.cpp`,
  `KNOWN_UNLINKED`, tracked on #622/#631) ⇒ **147 registered**.
- **87** match at least one batch; **39** match a required batch; **60** match none.
- 14 batches, 8 required. `ComputeInfrastructure` and `Streaming` match **0** cases each.

The 60 partition as follows:

| Issue | Group | Cases | Source |
| --- | --- | --- | --- |
| #906 | bare-tagged renderer pipeline | **38** | `test_renderer_pipeline.h` |
| #907 | device ownership / buffer lifetime / leak detection | **12** | `test_render_device_manager_ownership.h` (7), `test_memory_leak_detection.h` (3), `test_gpu_buffer_manager_lifetime.h` (2) |
| #908 | streaming — **fixed by retag, not by declaration** (§5.2) | **3** | `test_gpu_streaming.h` |
| #910 | singletons | **6** | composite-hazard (2), importer (1), sorting-perf (1), phase-1 integration (1), render validation (1) |
| — | *(not in the split)* | 1 | `test_tile_renderer.cpp` — inside the `TileRenderer` filter, subtracted by the honored #643 `excludes` |

38 + 12 + 3 + 6 = **59**, exactly the count the wildcard declares **today**. This is the
partition of the corpus as it stands *before* T4 acts; §5.2's retag then moves #908's three out
of the stranded set entirely, so the manifest T4 *lands* declares 56 across three entries, not
59 across four. §5.4 works this through — the pre-state and the post-state are different
numbers, and conflating them writes a manifest the guard rejects.
(`tests/ci/quarantine_manifest.json`: `*][RequiresGPU]*`, `count: 59`, issue #820, expiry
`2026-10-15`). The 60th unbatched-at-runtime case is the #643 waiver — a filter exclusion with
its own `deferred_requires_gpu_waivers` entry, not a member of the split and not this task's
work. #909 contributes **0** cases to the partition, for the reason in §5.3.

### 5.2 `Streaming` is a retag, not a filter edit

The filter is `*Streaming*][RequiresGPU]*`, which requires the token to appear **before** the
literal `][RequiresGPU]`. All three cases in `test_gpu_streaming.h` are tagged
`[GaussianSplatting][RequiresGPU]` and mention streaming only in the descriptive tail, so the
filter matches none of them. This is a tag-**order** defect.

**Decision: add the `[Streaming]` subsystem tag before `[RequiresGPU]` on the three cases.**
Not a widened filter — one of the three, `Stage-B instance depth culling toggles`, carries no
`Streaming` token anywhere in its name, so no filter edit could reach it. A filter loose
enough to try (`*Streaming*`, matching the descriptive tail) would instead pull in cases from
`test_renderer_pipeline.h` that belong to #906's group and from
`test_scene_director_submission_scaffolding.h` that are already batched — it would move the
boundary rather than fix it, and it would make batch membership depend on prose.

Per §2.5, the acceptance evidence for this retag is the derived membership set before and
after — the *corpus* count is identical on both sides by construction, because a retag is a
rename.

**But the retag changes the stranded set, and therefore the manifest.** This is the half that
§2.5's "a retag is a rename" warning is easy to under-read. `check_test_lane_coverage.py`
computes `gpu_hit` against **every** batch in `run_gpu_harness.py`'s `BATCHES` — advisory
batches included, not just `REQUIRED_BATCHES` — and a case with `module_hit or gpu_hit` is not
stranded (`:538-551`). The whole point of this retag is to make these three cases match the
`Streaming` batch, so the retag **un-strands them**. Measured at `adcd6916dbd` by driving the
guard's own `_doctest_wildcmp` against `BATCHES`:

| Case (post-retag name) | Matches a batch? |
| --- | --- |
| `[GaussianSplatting][Streaming][RequiresGPU] GPU Memory Streaming` | `Streaming` |
| `[GaussianSplatting][Streaming][RequiresGPU] GPU Memory Streaming Performance` | `Streaming` |
| `[GaussianSplatting][Streaming][RequiresGPU] Stage-B instance depth culling toggles` | `Streaming` |

All three match `Streaming`'s filter `*Streaming*][RequiresGPU]*` after the retag and none
before. So the stranded `[RequiresGPU]` population drops **59 → 56** the moment the retag lands,
and #908 needs **no** manifest declaration — see §5.4.

### 5.3 `ComputeInfrastructure` is deleted, and retagging is rejected

All **11** `[ComputeInfra]` cases live in `test_compute_infrastructure.h` and **none** carries
`[RequiresGPU]`. They are null-device CPU tests: they pass `nullptr` as the `RenderingDevice`
and assert the error paths that produces (`:134`, `:137` —
`CHECK(result.detail.contains("RenderingDevice is null"))`).

**Decision: delete the `ComputeInfrastructure` batch. Retagging is explicitly rejected.** Two
reasons, and the second is the decisive one:

1. Tagging a null-device test `[RequiresGPU]` is a **false claim about what it exercises**. The
   batch would then run green on a GPU runner while proving nothing about the GPU — the
   permanently-green catalogue entry replaced by a permanently-green batch.
2. It would **remove existing coverage**. `run_module_tests.py:104` runs
   `("GaussianSplatting [ComputeInfra]", ("*GaussianSplatting*][ComputeInfra]*",),
   ("*][RequiresGPU]*",), True)` — a **strict** lane that explicitly *excludes*
   `[RequiresGPU]`. Adding the tag would evict all 11 cases from the strict lane that runs
   them today.

Deleting the batch therefore loses nothing and removes a hollow entry. This rejection is
recorded here so it is not "restored" later by someone reading only the zero-match symptom.

### 5.4 #820 closes **by** the split; `REQUIRED_BATCHES` stays enumerated

- **#820 is closed by the split, as superseded by #906–#910.** Its subject — the 59-case
  wildcard declaration — ceases to exist, and an open issue pointing at a declaration that is
  gone is worse than no issue. **The wildcard is replaced by three named declarations — #906
  (38), #907 (12) and #910 (6), summing to 56** — carrying the same required fields the manifest
  already demands (`UNLANED_REQUIRED_FIELDS`: `test_case`, `count`, `reason`, `issue_url`,
  `owner`, `expires_utc`).
- **Two of the five children declare nothing, for two different reasons. Both would fail the
  guard if declared.**
  - **#909** — its batch is deleted outright (§5.3), so it contributes **0** cases to the
    partition (§5.1), and a zero-count declaration cannot exist:
    `tests/ci/check_test_lane_coverage.py:380` rejects any `unlaned_tests` entry whose `count`
    is not a positive integer
    (`if not isinstance(entry.get("count"), int) or entry["count"] < 1`). #909's work is the
    batch deletion.
  - **#908** — its three cases stop being stranded the moment §5.2's retag lands, because they
    then match the `Streaming` batch and `gpu_hit` is computed over all `BATCHES`. A #908
    declaration would match **zero stranded cases** and fail as **stale**: *"matches NO
    currently stranded test case… the tests were given a lane"* (`:605-610`). #908's work is
    the retag, and the retag is the thing that removes the need to declare it.

  Both failures are the same shape from opposite ends — a declaration is a statement that cases
  are stranded, and neither set is. **The count the manifest can honestly carry after T4 is 56,
  not 59.** An implementer who preserves the 59 total by keeping a #908 or #909 declaration has
  written a manifest the guard rejects; one who preserves it by inventing a fourth group has
  written a partition this ADR does not describe.
- **Ordering, because 59 → 56 is a live constraint, not bookkeeping.** The wildcard declares 59.
  If the retag lands while the wildcard still exists, the wildcard matches 56 and the guard
  fails it for over-declaring (`:619-624`, *"matches 56 stranded case(s) but declares 59…
  lower 'count' to 56 so the slack cannot be reoccupied"*).
  Either the retag and the wildcard's count drop land together, or the retag and the full split
  land together. §9.1 states the atomicity requirement.
- **`REQUIRED_BATCHES` stays policy-enumerated, not derived.** It records a *decision* ("this
  batch must be able to fail CI"), and that is precisely the fact a tree which has already
  lost the batch can no longer tell you — the same reasoning
  `check_test_lane_coverage.py`'s `STRICT_COVERAGE_CONTRACTS` docstring gives for its own
  enumeration. Everything else in T4 is derived: counts, memberships, the partition itself.

### 5.5 A proven-green GPU run precedes any case entering a required batch

**Decision:** the #724 bar, restated as a rule rather than a precedent. Before any batch
enters `REQUIRED_BATCHES`, it must have run on the self-hosted GPU runner from a real build
with `>0` cases, `>0` assertions, `case_assert_audit_ok=true`, `zero_assertion_cases=[]`, and
recorded wall-time headroom against its budget.

**Corollary: none of #906–#910 promotes anything.** A retag moves cases into an *advisory*
batch; promotion is a separate, separately-evidenced step. Landing a retag and a promotion in
one PR would mean requiring a batch nobody has yet watched run.

## 6. T10 (#903) — production-defaults pixel coverage and advisory expected-fail scenes

The QA project pins `gaussian_splatting/composite/depth_test=false`
(`tests/examples/godot/test_project/project.godot`, `[rendering]`) while the shipped default is
`true` (`gaussian_splat_manager.cpp:998`). Phase-0 runtime evidence, on a binary built from
`55bd3953475`:

| Config | `scaling_3d` | scale | `depth_test` | Splats |
| --- | --- | --- | --- | --- |
| default-default | Bilinear | 1.00 | **true** | **PRESENT** |
| scaled | Bilinear | **0.75** | **true** | **ABSENT** (`GS-AUDIT-GPU-001`) |
| temporal | **FSR2** | 1.00 | **true** | **ABSENT** (`GS-AUDIT-GPU-001`) |

### 6.1 The default-default scene lands blocking, now

**Decision.** scale 1.0 + `depth_test=true` is green today, so it joins `test_scenes` in
`qa_test_runner.gd` with a captured baseline. No ladder, no advisory step — a scene that
passes on the first run belongs in the blocking suite immediately, and this one closes the
single largest blind spot in the only per-PR pixel gate.

### 6.2 The two red scenes land as advisory expected-fail oracles

**Decision.** A new `EXPECTED_FAIL_SCENES` map beside `QUARANTINED_SCENES` in
`qa_test_runner.gd`, carrying the same fields the quarantine manifest requires (reason, issue,
owner, expiry) **plus a required `expected_failure` signature**. Five behaviours, all pinned:

| Observed | Outcome |
| --- | --- |
| scene runs, **RED, matching** the entry's `expected_failure` | recorded as `expected_fail`, counted in the summary and in `qa_results.json`, **suite does not fail** |
| scene runs, **RED, not matching** the entry's `expected_failure` | **suite fails**, reported as `unexpected_failure` with both the expected and the observed signature printed |
| scene runs, **GREEN** | **suite fails.** Either GPU-001 is fixed — remove the entry and promote — or the scene stopped asserting. Both need a human |
| scene produced **no result** | fails closed, as a missing quarantine entry does today |
| entry's **expiry passes** | fails, matching the quarantine manifest's clock-checked expiry |

**Why the signature is required, and not optional.** Without it the table classifies *any* red
as `expected_fail`, and the suite is then blind in the one direction that matters most: a scene
that still exhibits GPU-001 **and** develops a second, unrelated regression looks exactly like a
scene that only exhibits GPU-001, and stays advisory indefinitely. So does a scene that stops
failing for GPU-001's reason and starts failing for a brand-new one. The green-fails-the-suite
row catches *repair*; it cannot distinguish the known failure from a new one, and nothing else
in the table can either. Reading "it is red, as expected" off a red that was never checked
against what was expected is precisely the shape
[`evidence-integrity.md`](../governance/evidence-integrity.md) forbids — absence of a
distinguishing signal read as a passing signal — and it is the same shape as the `[RUNTIME_PASS]`
gap §4 exists to close, one lane over.

**What the signature is, and the judgement behind it.** **Pin a stable failure reason /
assertion identity, not a pixel fingerprint.** Concretely, the entry declares the identity of
the assertion the scene is expected to fail on — a stable machine token the scene emits
alongside its human message, in the same spirit as the existing `[QA_SKIP]` marker
(`qa_test_base.gd:29`) — optionally narrowed by a predicate over the metrics the runner already
carries through `get_result_metrics()` / `append_renderer_diagnostics()`
(`qa_test_base.gd:109,113-158`, which already surfaces `visible_splats` and `sorted_splats`).
GPU-001's signature is "**splats absent**", i.e. a splat-count-zero condition on the scene's own
content assertion.

The alternative — hashing the failing frame and matching the hash — is rejected. A pixel
fingerprint over a *visual mismatch* is brittle in exactly the wrong direction: driver
revisions, unrelated shader changes, tone-mapping tweaks and ordinary sampling noise all move
the bits without changing what failed, so the guard would emit false `unexpected_failure` reds
for changes that have nothing to do with GPU-001, and the pressure that follows is to delete the
signature rather than fix anything. An assertion identity is stable under everything except a
change to *why* the scene fails, which is the only event the signature is meant to detect.

**One thing the T10 implementer must measure rather than assume.** Which stage GPU-001's
zero-count is observable at is *not* settled here. The defect is conditioned on
`composite/depth_test=true`, so it may well be a composite-stage loss with `visible_splats > 0`
upstream — in which case the signature must key on the scene's own content assertion and not on
`cull_gpu_visible_count`. T10 records the observed metric values for both red configs and pins
the signature against what it measured, not against what this paragraph guessed.

**Why not quarantine.** A quarantined scene does not run and therefore cannot observe its own
repair. An expected-fail scene runs every time and is a live regression oracle for the fix.

**Why the suite cannot be mistaken for broken.** The summary distinguishes `failed` from
`expected_fail`, prints the issue reference for each, and a suite whose only red is
expected-fail exits 0 with one line naming the scenes and their issues. A reader who sees red
scene output and a zero exit code is told, in the same output, why.

### 6.3 Expected-fail crosses into the Python lane, and execution is not inventory

**§6.2 specifies a GDScript-side map and stops at the language boundary. Four mechanisms —
three in the Python lane, one in the runner itself — reject the design as written.** Read at
`adcd6916dbd`:

1. **The comparator fails on any scene lacking a committed baseline.**
   `run_baseline_qa.py:1138` computes `comparison["new_scenes"]` as current-minus-baseline;
   `:1295-1299` folds it into `has_regressions = bool(regressions) or bool(missing_scenes) or
   bool(new_scenes)`; `:1300` sets `comparison["status"] = "failed"`, and `:1308-1309` prints
   *"Current QA scene has no committed baseline"*. Adding the two red scenes to `test_scenes`
   therefore reds the lane **even when `qa_test_runner.gd` exits 0** — §6.2's exit-code design is
   necessary and not sufficient.
2. **They cannot simply be baselined.** `validate_baseline_candidate()` (`:206`) rejects any
   entry with `if not entry.get("passed", False):` (`:236`) — *"a failing run must never become
   the baseline."*
3. **The inventory guard knows exactly two buckets.** `_qa_inventory_failures`
   (`tests/ci/test_baseline_qa_require_flag.py:68`) derives `var test_scenes:` and
   `const QUARANTINED_SCENES` from the runner source by regex (`:71-83`) and then asserts: each
   scene declared **exactly once** across the two (`:86-89`), the two are disjoint (`:94-95`),
   every scene file on disk is in one of them (`:96-97`, *"QA scene files neither active nor
   quarantined"*), no declared scene is missing from disk (`:98-99`), and the committed
   baseline's active and quarantine sets **equal** the runner's (`:103-118`). A third GDScript
   map is invisible to all of it: put the red scenes only in `EXPECTED_FAIL_SCENES` and their
   files trip `:96-97`; put them in `test_scenes` and the baseline-equality check at `:108-112`
   demands baseline entries that (2) forbids.

**None of this is a bug to route around.** `validate_baseline_candidate`'s refusal is **a guard
doing precisely its job**, and the inventory guard's disk-coverage rule is what stops a scene
quietly leaving the suite — the same property `_strip_machine_dependent_metrics`' docstring
protects when it keeps a metric-less entry so *"the comparator's `missing_scenes` check fails if
the scene silently drops out"* (`run_baseline_qa.py:303-310`). **Do not weaken any of the three.**

4. **And execution membership is `test_scenes` alone.** `_run_next_test()`
   (`tests/examples/godot/test_project/scripts/qa_test_runner.gd:94`) bounds on
   `test_scenes.size()` (`:98`) and indexes `test_scenes[current_test_index]` (`:101`). Nothing
   else runs. A scene in a third map and not in `test_scenes` **never executes** — so it cannot
   be an oracle at all, which is the entire purpose of §6.2.

**The fourth trap defeated this section's own first answer.** The round-4 revision required
three *pairwise-disjoint* inventory buckets while also saying `test_scenes` grows by two — which
cannot both hold: disjointness forbids the expected-fail scenes from being in `test_scenes`, and
`:98`/`:101` mean that anything not in `test_scenes` never runs. There was **no valid
representation** under the rules as written. That is §2.6's eighth instance and the third
introduced by a correction.

**The resolution is a distinction the section had collapsed: execution membership and inventory
membership are different questions.**

- **Execution membership** answers *what does the runner iterate?* It must be the **union** of
  active and expected-fail scenes — expected-fail scenes have to run every time, or they cannot
  observe their own repair (§6.2's "why not quarantine").
- **Inventory membership** answers *which declared bucket owns this scene, and is every scene on
  disk owned exactly once?* Here the buckets stay **disjoint and total**, which is what makes
  `:96-97` and the exactly-once rule at `:86-89` meaningful.

A scene is therefore in exactly one *bucket* and may be in more than one *derived set*. The
runner's iteration order is **derived from** the buckets rather than being one of them — which
is also the only shape consistent with §2's derive-don't-enumerate posture, since a
hand-maintained `test_scenes` that must stay in sync with a second map is precisely the
drift this cluster exists to remove.

**Invariant, and it is what this section decides.** Whatever representation T10 chooses must
satisfy all five at once:

1. **Expected-fail scenes execute on every run** — union semantics for the executed sequence.
2. **Every scene file on disk is owned by exactly one declared bucket**, so `:96-97` and
   `:86-89` stay total rather than acquiring a hole.
3. **`validate_baseline_candidate` never sees a `passed=false` entry** (`:236`) — its refusal is
   a guard doing its job and is **not weakened**; the representation carves around it.
4. **A scene that is neither baselined nor declared expected-fail still fails** `new_scenes`
   (`:1295-1300`) — the property that check exists for survives.
5. **Runner and baseline disagreeing is a failure**, on the equality model `:103-118` already
   uses. Silence between the two sides is never agreement.

**Precedent to build on, not a mandate:** the committed baseline already carries a `quarantined`
map *beside* `results` (read at `test_baseline_qa_require_flag.py:106`), precisely so
quarantined scenes can be inventoried without being baselined. An `expected_failures` map in
that shape satisfies (3) and (5) naturally, and reuses the programme's one waiver idiom (§2.7,
§6.6). T10 may choose otherwise; it may not choose something that fails the five.

**Sequencing.** The Python changes land **before or with** the GDScript map, never after: the
moment the executed set grows by two scenes with no committed baseline, the blocking lane is red
at `:1300`. This is T10's own ordering constraint, and it is why §1's T10 row is not "scenes plus
`qa_test_runner.gd`" work.

### 6.3.1 What T10's PR owes for the delegated representation

Per §2.7, the exact bucket layout, field names, and the mechanism composing the executed
sequence are **T10's PR to decide**. This section has specified that representation twice and
been wrong twice, and the remaining questions are ones a run of the QA lane answers immediately.
In exchange, T10's PR must record:

1. **The representation chosen** — which buckets exist, which is derived, and how the executed
   sequence is composed from them.
2. **A run showing all five invariants hold together**: the two red scenes executed and reported
   `expected_fail`; the suite exit code 0; `_qa_inventory_failures` green; and
   `validate_baseline_candidate` still rejecting a `passed=false` entry when handed one.
3. **A mutation proof per §2.4** — remove one expected-fail declaration and show the lane goes
   RED, so the representation is demonstrably load-bearing rather than decorative.

**Inadequate:** an inventory-guard pass without a run that actually executed the two scenes
(that is the round-4 defect exactly — a representation that inventories cleanly and never
runs); or a green suite without showing `validate_baseline_candidate` still refuses
`passed=false`, since the cheapest way to make all of this green is to weaken the guard the
section forbids weakening.

### 6.4 The entries point at GPU-001's own issue, filed at T10 time

**Decision (settled: §11 Q2). `GS-AUDIT-GPU-001` gets its own GitHub issue, filed when T10 is
implemented, and the `EXPECTED_FAIL_SCENES` entries point at that issue — not at #903.** The
issue is **not** filed now; T10 files it, and this ADR records only that it must exist before
the entries do.

Two reasons, and the second is the operative one:

- **Filing it is inside the Phase-1 exclusion policy, not an exception to it.** Phase-2/3
  findings were kept out of the Phase-1 issue batch because they were not being worked. The
  moment an in-repo artifact — the expected-fail oracle — references the defect, GPU-001 *is*
  active work: something in the tree now depends on the defect's identity and on someone
  noticing when it changes. That is the condition the exclusion was drawing the line at. It
  also costs nothing to write: GPU-001 is the best-evidenced finding in the audit —
  runtime-confirmed with the pixel matrix reproduced above — so the issue body comes straight
  out of the ledger.
- **Pointing at #903 would tangle two different closure events.** **#903 closes when the gate
  covers production defaults.** **The oracles flip to blocking when the defect is fixed.**
  Those are different events with different owners and different evidence, and a tracker can
  only mean one of them. Under the "re-point later" option, #903 stays open purely as a
  placeholder for a defect it does not describe, or it closes and the manifest is left
  pointing at a closed issue — which is the same shape §5.4 rejects for #820. Different
  events need different trackers.

### 6.5 The flip to blocking is gated on GPU-001 being fixed, not on a date

Removing an `EXPECTED_FAIL_SCENES` entry is the whole flip — the same property
`QUARANTINED_SCENES` already documents ("Removing an entry is the whole fix for that scene").
No separate promotion machinery is built for a two-entry map. The trigger is GPU-001's issue
closing (§6.4), which is why that issue and #903 must be distinct: #903 is done long before
this flip is possible.

### 6.6 The override-diff guard — the other half of `TEST-008`

`GS-AUDIT-TEST-008`'s remediation direction has **two** clauses, and §§6.1–6.5 take only the
first: *"Add a production-defaults QA scene (depth_test=true, default lighting) to the blocking
suite; **add a guard diffing test-project overrides against shipped defaults with an explicit
waiver list**."* The second clause is specified here. It was carried in T10's task from the
start and was under-specified in this record, not out of scope — and dropping it would silently
shrink the `TEST-008` fix to half its remediation.

It is also the clause that addresses the finding's own root cause. `TEST-008`'s
`why_existing_tests_or_gates_miss_it` reads: *"The gate compares against baselines captured
under the same non-default config, so both sides drift together; **nothing diffs QA-project
settings against manifest/GLOBAL_DEF defaults.**"* Scenes close the two configurations we
already know about; the guard is what stops the next divergence from being discovered by an
audit. The surface is larger than `depth_test`: the QA project carries **32**
`gaussian_splatting/*` overrides (`tests/examples/godot/test_project/project.godot`,
`[rendering]`), of which `TEST-008` names six as material — `composite/depth_test=false`,
`effects/max_effectors=0`, `lighting/shadow_strength=0.0`, `lighting/indirect_sh_scale=0.0`,
`quality/tier_apply_streaming_budgets=false`, `streaming/auto_regulate_enabled=false`.

**Four requirements, and they are the specification.**

1. **Both sides derived; neither side hand-written.** Overrides are parsed from the QA
   `project.godot`; defaults come from the registrations that declare them, cross-checked for
   key completeness against the 199-key inventory in
   `modules/gaussian_splatting/config/project_settings_manifest.json`
   (`root_prefix: rendering/gaussian_splatting/`). The manifest is a key inventory and does
   **not** carry default *values*, so values must come from the registrations and the manifest
   is what catches an override whose key is registered nowhere — which fires today:
   **`gaussian_splatting/instance_pipeline/enabled=true` is overridden by the QA project and
   has no manifest entry** (31 of the 32 overrides are inventoried). **A hand-written table of
   expected defaults is forbidden** — it is the same defect this cluster exists to remove, and
   it would rot silently the first time a default changed.

   **`GLOBAL_DEF` is not the only registration form, and a `GLOBAL_DEF`-only guard would be
   vacuous over the keys that matter most.** An earlier revision of this section required
   defaults to come "from the `GLOBAL_DEF` registrations". **That requirement originated with
   the maintainer**, written without reading the registration mechanisms — and written while
   formulating §2.6, the rule against exactly this. It was relayed unverified, caught by the
   Codex review pass, and confirmed against the source by the maintainer. It is recorded that
   way rather than as an anonymous "earlier revision" because §2.6 binds every layer, and the
   only evidence that it does is a case where it caught the top one. Measured at `adcd6916dbd` by
   resolving manifest keys against string literals at `GLOBAL_DEF*` call sites: only **128 of
   199** resolve, and of the QA project's 32 overrides **14 do not resolve at all** — including
   **all three lighting keys**, which is precisely the divergence `TEST-008` calls material. The
   forms actually present in the module:

   | Form | Sites | Example |
   | --- | --- | --- |
   | `GLOBAL_DEF` | 189 | `GLOBAL_DEF("rendering/gaussian_splatting/composite/depth_test", true)` — `core/gaussian_splat_manager.cpp:998` |
   | `GLOBAL_DEF_RST` | 1 | `core/gaussian_splat_manager.cpp:1065` |
   | `set_setting` + `set_initial_value` | 3 files | `renderer/gaussian_splat_renderer.cpp:189` `_initialize_lighting_project_settings_defaults()`, called at `:883`; `renderer/quantization_config.cpp:210-212`; `renderer/sh_config.cpp:139-141` |
   | `GLOBAL_DEF` with a **constructed** key | 23 paths | `GLOBAL_DEF(GPUSortingConfig::MAX_ELEMENTS_PATH, …)` — `renderer/gpu_sorting_config.cpp:690`, key built at `:24` |

   **And the unresolved keys have two distinct causes, which matters because only one of them
   is about registration forms.** Reading the actual sites:

   - **A different form.** The lighting keys are registered with no `GLOBAL_DEF` anywhere in the
     path (below).
   - **A constructed key.** `renderer/gpu_sorting_config.cpp` builds **23** setting paths by
     concatenation — `const String GPUSortingConfig::MAX_ELEMENTS_PATH = SECTION_PATH +
     "max_sort_elements"` (`:24`, with `SECTION_PATH` at `:20`) — and then registers them with
     the ordinary macro: `GLOBAL_DEF(GPUSortingConfig::MAX_ELEMENTS_PATH, 50000000)` (`:690`).
     The form is `GLOBAL_DEF`; the full key literal simply never appears in the source, so no
     literal scan can find it however many forms it enumerates.

   A third obstacle sits behind both: some registrations take a **runtime value**, e.g.
   `GLOBAL_DEF(GPUSortingConfig::BOUNDED_BUFFER_SHRINK_PATH,
   g_gpu_sorting_config.bounded_buffer_shrink_enabled)` (`:701`). Even with the key resolved,
   the default is not a literal to read. Source-scraping therefore has to solve key
   constant-folding *and* value constant-folding, not just form enumeration — which is the
   strongest argument for querying at runtime, where all three collapse into one lookup.

   (The same file shows why this matters beyond the guard: `:137-140` carries hand-written
   fallbacks with the comment *"Fallbacks MUST match the registered `GLOBAL_DEF`"* — a
   duplicated-default coupling of exactly the kind rule 5 exists to catch. Out of scope for
   T10; noted because the guard would be well placed to catch it later.)

   The lighting path is the worked example, and it contains no `GLOBAL_DEF` at all:
   `direct_light_scale` 0.5 (`:203`, `:205`), `indirect_sh_scale` 1.0 (`:208`, `:210`),
   `shadow_strength` 1.0 (`:213`, `:215`), each guarded by `has_setting` and pinned with
   `set_initial_value`. Against those, the QA project sets `indirect_sh_scale=0.0`,
   `shadow_strength=0.0` **and `direct_light_scale=1.0` against a 0.5 default** — a third
   lighting divergence `TEST-008` does not name, found only because the values were read.

   **Requirement: cover every authoritative registration mechanism, or query the registered
   defaults at runtime** rather than scraping source. Querying sidesteps the enumeration problem
   entirely — every form above ends in the same `ProjectSettings` store — and is preferable
   wherever the guard can boot the engine.

   One caveat the implementer must price in rather than discover, because it decides the
   approach: **the value the guard needs is the registered *initial* value, and there is no
   public C++ accessor for it.** `core/config/project_settings.h` exposes
   `set_initial_value` (`:172`) but no getter; the stored value is reachable only through
   `_property_get_revert` (`core/config/project_settings.cpp:1300-1307`, returning
   `value->value().initial`), declared in the non-public section at `:118-119`.
   `get_setting_with_override` (`:204`, the target of the `GLOBAL_GET` macro at `:254`) returns
   the *effective* value — which in the QA project is the override itself, making it useless
   for this comparison. So the runtime route needs either the bound property-revert path or a
   new engine accessor; the latter is `core/**` and therefore **R3 engine-delta** with its own
   review burden. If that price is not worth paying, the source-scraping route is legitimate —
   it just has to enumerate all the forms above *and* fail closed on the rest.

   **And the load-bearing half: fail closed when a manifest key has no derived value.** An
   unresolved key is reported and RED, never skipped. Without that clause the guard silently
   passes over exactly the keys it could not parse — which today would be 71 of 199, the
   vacuous-guard shape this ADR exists to eliminate, rebuilt inside the guard meant to close it.

   **The list of forms above is open-ended, not closed.** It is what a grep of the module found
   at `adcd6916dbd`; a form added later would not appear in it. That is why the fail-closed
   clause, not the table, is the requirement — the table tells an implementer where to start,
   and the unresolved-key failure is what catches the ones the table misses.

   **But as stated, that fail-closed rule can never go green — and that is a defect of a kind
   this programme has not seen before.** Some keys are *intentionally* unregistered. At
   `modules/gaussian_splatting/config/project_settings_manifest.json`,
   `rendering/gaussian_splatting/debug/force_unclustered_lights` carries
   `"effective_state": "none"` and a note reading *"Live raw-only debug hook (**unregistered, no
   `GLOBAL_DEF`**): read via `gpu_debug_utils.h` `is_debug_force_unclustered_lights_enabled()`"*.
   No registration exists to scrape and no initial value exists to query, so **neither** source
   in requirement 1 can resolve it, and a rule that reds on every unresolved key reds forever.

   **Name the class: this is a vacuous FAIL, the mirror of the vacuous pass.** Every other
   finding against this document was a gate that could wrongly *pass*; this is a gate that can
   only *fail*. It is not the safe direction. A gate that cannot go green is deleted, or
   `# noqa`'d, or "temporarily" narrowed by whoever meets it next — and it protects nothing in
   the meantime, because a signal that is always red carries no information. Both failures are
   the same underlying error, a guard whose output does not vary with the thing it guards, and
   §2.1's "fail closed" is not a licence to build one: a guard must have **both** a reachable
   RED and a reachable GREEN, and the acceptance evidence per §2.4 must show both.

   **Fix: exempt keys the manifest itself declares unregistered, derived from the manifest,
   never from a hand-written key list.** The exemption must be a property of the inventory, so
   that adding an unregistered key is a manifest edit rather than a guard edit.

   **Which field is authoritative — measured, and the answer is "none of the existing ones."**
   This is stated rather than resolved by picking the field that happens to work, because the
   measurement says picking one would silently break the guard:

   | Candidate | Keys matched | Verdict |
   | --- | --- | --- |
   | `effective_state == "none"` | 24 | **Wrong.** 19 of the 24 *are* `GLOBAL_DEF`-registered (`enable_frame_logging`, `frame_log_frequency`, `eager_raster_pipeline`, the `cull_guardrail_*` family…). The field means "maps to no effective-state field", which is a statement about state plumbing, not about registration. |
   | `publicness == "debug_only"` | 26 | **Wrong**, and sparse: `publicness` is present on only 49 of 199 entries, inherited from `families` for the rest. |
   | `test_coverage` | 199 | Orthogonal — it describes test status, not registration. |
   | `notes` free text | 1 | The **only** place the fact is actually recorded, and not machine-readable. |

   Choosing either plausible field would exempt **two of the 32 QA overrides the guard exists to
   check** — `debug/enable_frame_logging` and `debug/frame_log_frequency`, both registered, both
   overridden by the QA project — which is the "picked a field that works today" failure
   happening immediately rather than later.

   **So T10's work includes a manifest schema addition**: an explicit, machine-readable
   `registration: "unregistered"` (or equivalent single-purpose field) on the entries that are
   deliberately raw-only, set on `force_unclustered_lights` and on any sibling found by the same
   audit. The guard exempts exactly the keys carrying it, fails closed on every other unresolved
   key, and — per §2.5 — **an exemption that no longer matches a raw-only key fails as stale**,
   so the list cannot outlive its reason. `project_settings_manifest.json` is inside the module
   and `modules/gaussian_splatting/tests/check_project_settings_manifest.py` already guards its
   shape, so this is an in-scope edit for T10 rather than new surface.
2. **The waiver list carries a per-entry reason, in the same shape as §8.3's exclusion list.**
   One waiver idiom for this programme, not three: the quarantine-manifest shape — the keyed
   subject, `reason`, `issue_url`, `owner`, `expires_utc`. The two lists differ in subject only
   (a settings key here, a path glob in §8.3) and **must not** differ in mechanism; if an
   implementer finds a reason they must diverge, that reason goes in both docstrings or the
   shapes stay identical. §8.3's membership-pinning requirement applies here too, in its
   degenerate form: a waiver names exactly one settings key, so "matches nothing" and "matches
   more than declared" collapse to "the key is not overridden any more", which fails as stale.
3. **An un-waived override is RED.** Any `gaussian_splatting/*` key whose QA value differs from
   its shipped default and which carries no waiver fails the guard. Not a warning, not a
   report line — the whole point is that the current 32 were never a decision anyone recorded.
4. **The guard registers itself, per §7.** It is a new `tests/ci/check_*.py`, so it adds itself
   to policy's "CI deterministic-check / release-gate machinery" enumeration in the same diff,
   which floors T10's PR at R3 via `SELF_REFERENTIAL_PATHS`. This is what makes T10 the second
   worked instance of §7's programme-wide rule rather than an assertion about a guard that does
   not exist: §6.6 is the thing §7 points at.

**Landing posture.** The 32 existing overrides are pre-existing, and this ADR does not decide
which of them are legitimate — that is the guard's first PR, arguing each waiver on its merits.
What is decided here is that the arguing happens **once, in the open, with expiries**, rather
than never. The advisory-then-blocking ladder is available if the first pass is large; what is
not available is landing the guard with a blanket waiver, which would reproduce the wildcard
§5.4 deletes.

**Class.** Specifying the guard changes T10's measurement, and per the programme principle the
class follows the design rather than the design being trimmed to hold a class. Measured at
`adcd6916dbd`:

```
$ python scripts/agentic/classify_change.py --paths \
      tests/examples/godot/test_project/scripts/qa_test_runner.gd \
      tests/ci/check_qa_settings_overrides.py .agentic/policy.json
risk_class: R3
  R1  tests/examples/godot/test_project/scripts/qa_test_runner.gd  (Local module or test change)
  R1  tests/ci/check_qa_settings_overrides.py  (Local module or test change)
  R3  .agentic/policy.json  (risk policy change (self-referential; forced to the top class))
```

So **T10 is R3 by requirement 4**, independently of whether it ever touches `baseline_qa.yml`.
§1's row and §11 Q5 are updated accordingly: Q5's question — does T10 reach the workflow? — is
no longer what decides the class, though it remains open as a design question. The guard file
name above is illustrative; only its directory and prefix are load-bearing.

**The later corrections did not move it.** §6.3's Python-lane work adds
`tests/ci/run_baseline_qa.py` (R1), and requirement 1's runtime route would add
`core/config/project_settings.h` (R3, engine delta). Both measured at `adcd6916dbd`; both
still R3 overall, so §1's row is unchanged:

```
$ ... --paths qa_test_runner.gd check_qa_settings_overrides.py run_baseline_qa.py .agentic/policy.json
risk_class: R3     # run_baseline_qa.py measures R1; the policy edit still sets the class
$ ... --paths check_qa_settings_overrides.py core/config/project_settings.h .agentic/policy.json
risk_class: R3     # now R3 twice over — engine delta AND the self-referential policy edit
```

Worth noting which way that cuts: the engine-accessor route does not *raise* T10's class,
because §7's self-registration already floored it. The reason to weigh that route is the
review burden of a `core/**` delta, not a class change.

### 6.7 Every QA/visual change is A/B'd on both `depth_test` values

**Decision, and it is a rule for the whole programme, not just T10.** Phase 0 measured that
the QA pin does not merely narrow coverage — it **inverts the result**: with
`depth_test=false`, splats present in *every* tested configuration — scale 1.0, scale 0.75 and
FSR2 alike — including the two that are broken at the shipped default. A test run under the QA pin
therefore returns a false *refutation* of the entire `depth_test=true` defect class, not a
weaker confirmation of it. Any QA or visual change in this cluster records both values, and a
result reported for only one value is not evidence.

## 7. New guards register themselves — a programme-wide rule, worked through T9 (#901)

**Rule (maintainer ruling, recorded here because it is design, not scope). Every
new `tests/ci/check_*.py` guard this programme creates adds itself to policy's "CI
deterministic-check / release-gate machinery" enumeration in the same diff that creates it.**
Not T9's alone. The consequence is deliberate and is the point: touching `.agentic/policy.json`
floors the PR at R3 via `SELF_REFERENTIAL_PATHS`, so a PR that introduces guard machinery is
graded as guard machinery from its first commit rather than from the first time someone
notices.

The rule generalises because the *reason* does. A guard is release-gate machinery on the day it
is written, not on the day someone remembers to enumerate it; the enumeration is what makes the
class true, and deferring it means the guard's own introducing PR — the diff where every design
decision inside it is made — is reviewed one or two classes below the machinery it is adding.
Splitting registration into a follow-up is the same move viewed from a different angle: it lands
an R3-grade guard under an R1 review.

**Instances in this cluster.**

- **T9 (#901)** — `tests/ci/check_engine_delta.py`. The worked example, measured below.
- **T10 (#903)** — the **override-diff guard specified in §6.6**, which is the second clause of
  `GS-AUDIT-TEST-008`'s remediation direction. T10's PR carries the `.agentic/policy.json` edit
  and the resulting R3 grade with it; §6.6 records the measurement. This is a worked instance
  rather than an assertion, because §6.6 now specifies the guard the rule attaches to.
- **No other member of §1 can be shown to introduce a new guard script from this ADR's text
  alone.** §9 records the remaining members as direct applications of their findings' remediation
  directions without naming files, so whether (for instance) T6's `validate_automation` wiring
  or T7b's orphan-probe work creates a new `tests/ci/check_*.py` is not determinable here. That
  is stated as a limit of this record, not as a finding that they do not. Each implementer
  checks the rule against their own diff; §10.1 is the guard that would make the check
  automatic.

### 7.1 T9 worked through, with the measurement

`tests/ci/check_engine_delta.py` does not exist yet (verified: no `*engine*` file under
`tests/ci/` at `adcd6916dbd`). As a new file under `tests/**` it matches only the R1 rule.
Measured:

```
$ python scripts/agentic/classify_change.py --paths ENGINE_PATCHES.md tests/ci/check_engine_delta.py
risk_class: R1
```

So the new guard that **polices R3 engine-delta drift** would itself be R1, because policy's
"CI deterministic-check / release-gate machinery" rule is a five-entry enumeration of
existing file paths and a new guard is not in it.

**Applying the rule: T9's PR adds its own new guard to that enumeration in the same diff.**
Adding it means editing `.agentic/policy.json`, and `SELF_REFERENTIAL_PATHS`
(`scripts/agentic/classify_change.py:57`, applied at `:129-133`, shipped in #886) forces any
diff touching that file to `ordering[-1]`. Measured:

```
$ python scripts/agentic/classify_change.py --paths ENGINE_PATCHES.md \
      tests/ci/check_engine_delta.py .agentic/policy.json
risk_class: R3
  R0  ENGINE_PATCHES.md                  (Docs / agentic governance)
  R1  tests/ci/check_engine_delta.py     (Local module or test change)
  R3  .agentic/policy.json               (risk policy change (self-referential; forced to the top class))
```

The PR that introduces the R3-policing guard is therefore graded R3 **by construction**, and
the gap closes in the same diff where it was found. The class is correct *because* the policy
edit rides along, not despite it — splitting the guard and its registration into two PRs would
land an R3-grade guard under an R1 review, which is the outcome this decision exists to
prevent.

The same mechanism, seen from the other side: **PR #911** declined to touch
`run_module_tests.py` precisely because that path alone flips a change from R1 to R3
"regardless of what the edit says", and its task was scoped R1. It documented the gap in the
guard's own docstring instead. Both decisions are right for their scope, and the pair is why
#898 is listed R3 in §1.

## 8. T7c (#897) — the GPU-evidence path filter is derived from policy's R2 globs

**Decision (settled: §11 Q3). T7c derives `renderStreamingPrefixes` from `.agentic/policy.json`'s
R2 `path_globs`, rather than continuing to hand-maintain it.** This is design content, so it is
made *here*, as the amendment §11 Q3 said it would need — not inside #897's PR.

### 8.1 The doctrine's own test, and how it answers

`.github/workflows/gaussian_production_gates.yml:165-169` hand-lists three prefixes and uses
them to decide whether the Windows GPU evidence lane runs at all
(`:198`, `path.startsWith(prefix)`). Policy's R2 rule lists eight globs.
[`evidence-integrity.md`](../governance/evidence-integrity.md) required practice 5 ("Derive
coverage; enumerate only policy") states the test verbatim: **would discovering a new item mean
the list is *stale*, or that someone must *decide*?** Applied here: when a new
`streaming_*.cpp` appears, is the prefix list stale, or does someone need to make a decision?

**Stale.** A file that policy already grades R2 is, by that grading, a change that requires
GPU/runtime evidence — the decision was made when the R2 glob was written. The prefix list is
not a second, independent judgement about which R2 paths deserve evidence; it is a copy of the
first judgement that nobody re-copies. `REQUIRED_BATCHES` (§5.4) stays enumerated for the exact
opposite reason: it records a decision the tree cannot reconstruct. This list records a decision
the tree *can* reconstruct, from the file that made it.

**This does not contradict §10.1**, which argues that policy's own R3 machinery rule must stay
enumerated. The two lists sit on opposite sides of practice 5's test. Policy's enumeration
*makes* the decision — which files are release-gate machinery — and deriving it from a filename
pattern would let a rename change a file's risk class. The workflow's prefix list makes no
decision at all; it *describes* the set policy already decided, which is why it can go stale
without anyone noticing. Derive the description, enumerate the decision.

**The drift is not hypothetical, and it is not small.** Measured at `adcd6916dbd` by expanding
policy's R2 `path_globs` over `git ls-files` and comparing against the workflow's three
prefixes:

- **197** tracked files are R2 by policy; the prefix list matches **152**.
- **45 R2 files are invisible to the evidence lane**: `core/` **32**, `compute/` **7**,
  `lod/` **4**, `asset_management/` **2**.
- The prefix set is a strict **subset** — **0** files match a prefix without also matching an
  R2 glob. So deriving only ever widens the lane; it cannot silently drop a path that is
  covered today.

The `core/` figure is the doctrine's point made concrete. Policy's glob is
`modules/gaussian_splatting/core/*streaming*`; the workflow's prefix is
`modules/gaussian_splatting/core/gaussian_streaming`. The prefix reaches
`gaussian_streaming.cpp/.h` and **nothing else**, while `streaming_atlas.cpp`,
`streaming_upload_pipeline.cpp`, `streaming_vram_regulator.cpp`,
`streaming_eviction_controller.cpp` and **28 more** sit in the same directory, are R2 by
policy, and change without ever arming the lane. Nobody decided that. The list was written when
`gaussian_streaming.*` was the streaming code, and the streaming code moved.

**Note the matcher difference.** Policy expresses globs (`**`, `*streaming*`); the workflow
tests `String.prototype.startsWith`. Deriving therefore means translating globs to a matcher,
not string-substituting a list — `core/*streaming*` has no prefix form. #897 owns that
translation, and its acceptance evidence is the derived path set compared against the 197-file
expansion above, per §2.5: a count that matches is not evidence that the membership matches.

### 8.2 The policy is read from the immutable base, never from the PR's own checkout

**Required, and it is the difference between a derivation and a self-serving one.** Deriving
from `.agentic/policy.json` **as checked out at the PR head** would let a PR that edits policy
alongside an R2 file narrow or delete the matching R2 glob, so `runWindowsGpu` stays false and
the GPU evidence lane never runs *for the very file that made the PR risky*. The change under
review would be deciding which evidence it owes. Editing `policy.json` does floor the PR at R3
(§7), but that is a *classification* control, not an evidence one: the gate's own summary states
that CI does not verify the published evidence was produced, so an R3 grade with a silently
skipped evidence lane is exactly the green-while-unobserved shape this cluster exists to remove.

**Decision: resolve the R2 globs from the immutable review base**, matching what the required
gate already does one layer up — `agentic_pr_gate.yml:105-116` reads
`git show "${GS_PR_BASE_SHA}:.agentic/policy.json"` and **fails closed** rather than classify a
PR against its own policy (*"refusing to classify this PR against its own policy"*). The same
argument applies unchanged to the evidence filter, so it gets the same treatment rather than a
new one.

If a PR legitimately widens the R2 surface and wants its own new paths covered in the same run,
the safe resolution is the **union of base and head** globs — never head alone. Union can only
add evidence; head-alone can remove it. Fail closed if the base policy cannot be read, for the
same reason the gate does.

### 8.3 Required: an explicit, reasoned exclusion list

**Deriving alone would make divergence impossible; that is the wrong goal. Divergence must be
*visible*.** Some R2 path may genuinely not need the GPU evidence lane — a header-only
constants file, a directory of pure CPU policy types — and a derivation with no escape hatch
either forces the lane onto it or invites someone to quietly narrow the R2 glob in
`policy.json`, which would be far worse: it would move the file's *risk class* to buy a cheaper
CI run. That is the "design follows class" inversion this ADR's programme principle names.

**Decision:** the derivation is `R2 globs − declared exclusions`, where each exclusion is a
committed entry carrying a **reason**, consistent with how this repo's other declared-exclusion
mechanisms are shaped. Both existing precedents are per-entry field maps, and the new one
follows them rather than inventing a shape:

| Mechanism | Per-entry required fields |
| --- | --- |
| `tests/ci/quarantine_manifest.json` `unlaned_tests` | `test_case`, `count`, `reason`, `issue_url`, `owner`, `expires_utc` (`check_test_lane_coverage.py:160-167`) |
| `renderer_release_gate_manifest.json` `deferred_requires_gpu_waivers` | `test_name`, `issue_url`, `owner`, `expires_utc`, `risk`, `mitigation`, `docs_path` (`check_renderer_release_gates.py:379-388`) |
| **T7c's exclusion list (new)** | `path_glob`, **`matched_paths`** (see 2), `reason`, `issue_url`, `owner`, plus `expires_utc` — see below |

Four properties the list must have, each taken from a precedent rather than argued from
scratch:

1. **Fail closed on a malformed or incomplete entry**, as both precedents do — a missing field
   is a validation failure, not a silently ignored exclusion.
2. **Each entry pins its matched membership, and both growth and shrinkage fail.** A
   zero-match check alone is not enough, and the earlier draft of this section got it wrong: it
   caught only the stale case, while the failure it *named in its own sentence* — "the next R2
   file that happens to match it inherits the exemption without anyone deciding" — slips
   straight through. A wildcard exclusion that still matches one existing file keeps passing a
   zero-match check while silently absorbing every new R2 file that matches it, which is the
   bypass the whole list is supposed to make visible.

   So each entry declares the **set of paths it currently excuses**, and the guard fails on any
   difference — a new file joining (an undecided exemption) or a file leaving (a stale licence
   whose slack can be reoccupied). The precedent is exact and already in this repo: the
   quarantine manifest's `count` does precisely this in both directions
   (`check_test_lane_coverage.py:611-624` — `actual > declared` is *"NEW stranded case(s) joined
   an already-declared family — a wildcard declaration must not silently amnesty cases written
   after it"*; `actual < declared` demands you *"lower 'count' … so the slack cannot be
   reoccupied"*). **Pin the membership, not merely the count**, per §2.5: a rename keeps the
   count identical while moving the set, and here a rename is exactly how a file would slide
   into an exemption unnoticed.
3. **Expiry.** `expires_utc` is carried, on the quarantine manifest's clock-checked model, so
   an exclusion is re-argued rather than inherited. **This is the one field where a case can be
   made either way** — an R2 path that genuinely never needs the lane is a permanent fact, and
   a permanent fact on a 90-day expiry is churn. The decision here is to carry it anyway,
   because "genuinely never" is exactly the claim that ages badly in this repo, and a re-argued
   exclusion costs one line. #897 may argue the other side in its PR; it may not simply omit
   the field.
4. **Prefer exact paths to wildcards.** Property 2 makes a wildcard safe, not cheap: every
   matching file must be enumerated in `matched_paths` anyway, so a glob buys nothing except a
   larger blast radius the day someone adds a file that matches it. A wildcard is available
   where a directory genuinely is excluded as a whole; the entry then still pins its members.

**Where it lives: inside `.agentic/policy.json`. This reverses an earlier position in this
section, and is recorded as a reversal rather than edited away.**

The earlier text forbade exactly this, on two grounds: that it would let a CI-cost argument be
settled by editing the risk-policy file, and that it would floor every subsequent exclusion at
R3 "for reasons that have nothing to do with the exclusion." **Both objections are answered by
taking them literally rather than avoiding them:**

- A CI-cost argument that reaches the risk-policy file is **precisely** the argument that should
  require R3 review. The objection described the mechanism working and called it a cost.
- Flooring a PR that grants itself an evidence exemption is **correct, not incidental**. The
  reasons have everything to do with the exclusion: an exclusion is a claim that a risk-graded
  path does not owe evidence, which is a risk-policy statement whatever file it is typed into.

**And the positive case is that co-location inherits both protections for free.**

- Anywhere else means **a second base-resolved artifact** — with its own base-resolution, its
  own self-referential handling, its own drift check. That is a second instance of the #886
  pattern to build and to get wrong, and this document's record on building the same protection
  twice is not good (§2.6).
- Inside `policy.json` the list is already covered by machinery that exists: §8.2 resolves policy
  from the immutable base, and `SELF_REFERENTIAL_PATHS`
  (`scripts/agentic/classify_change.py:57`) already floors any PR touching that file at R3.
- **§8.4's P1 then closes with no new rule at all.** The exclusion is read from base, so a PR's
  own addition cannot apply to itself; and adding it floored that PR at R3 anyway, so the
  evidence obligations it tried to suppress are published at maximum instead of skipped. The
  hole is closed twice over by machinery already in the tree.
- One hardened surface instead of two. Policy lives with policy.

**#897 may still deviate**, but only by writing down why, under §2.7's document-the-mechanism
obligation: the default is co-location, and a deviation must state what it gains that is worth
building base-resolution and self-referential handling a second time. "Beside the workflow is
tidier" is not that reason.

**What it is not.** The exclusion list is not a place to park R2 paths whose lane is currently
red. That is what the quarantine and waiver mechanisms above are for, and routing a red lane
through a coverage exclusion instead would hide it in the one list nobody reads as a failure
record.

### 8.4 The invariant: a change may add to the evidence it owes, never subtract

**This is the general rule, and §8.2 was only its first instance.** §8.2 pinned `policy.json` to
the immutable base so a PR could not narrow an R2 glob to excuse itself. §8.3 then introduced a
*second* input to the same decision — the exclusion list — and said nothing about where it
resolves from, which leaves it in the PR's own checkout. **The hole reopens one level down:** a
PR that adds an exclusion matching an R2 path *it also edits* suppresses its own
production-evidence collection, with formally valid metadata and a perfectly correct
`matched_paths`. Every property in §8.3 passes. This is the defect #886 closed for
`policy.json`, rebuilt inside the mechanism designed to replace a hand-maintained list — which
is precisely why it is stated here as an invariant rather than patched as a fourth bullet.

**Invariant.** *Every input to "which evidence does this change owe?" resolves in whichever
direction can only **add** evidence.* Concretely, comparing base and head:

| Input | Resolution | Why that direction |
| --- | --- | --- |
| R2 `path_globs` (inclusive) | **union** of base and head | union only ever adds paths to the lane |
| Exclusion list (subtractive) | **intersection** of base and head | a path is excused only if it was excused *before* this change too |
| Either input unreadable at base | **fail closed** | as `agentic_pr_gate.yml:105-116` already does |

Intersection is the exclusion-side mirror of §8.2's union, and it closes both directions of the
hole, not just the one Codex named:

- **A new exclusion** (present at head, absent at base) does **not** apply to the PR that
  introduces it. Evidence runs.
- **A removed exclusion** (present at base, absent at head) also does not apply — the path
  rejoins the lane immediately. Base-resolution *alone* would have missed this second case and
  suppressed evidence for a path the PR just un-excused, which is the same fail-open direction
  with the sign flipped.

**A legitimately new exclusion cannot excuse the PR that introduces it, and that is the intended
behaviour, not a side effect.** The author argues an exclusion in a PR that still pays the
evidence cost; the exclusion takes effect for everyone afterwards. That ordering is the whole
control: an exemption is a claim about the future, and the PR making the claim is exactly the
one whose evidence you least want to skip. An author who finds this expensive is describing the
mechanism working.

The alternative considered and not taken was **forcing the evidence lane whenever the exclusion
list changes at all**. It closes the same hole, but it is a second mechanism with different
semantics for the same question, and it is coarser — an unrelated exclusion edit would arm the
full GPU lane. Intersection reuses §8.2's idiom and reasoning exactly, which matters here: this
programme has now had the same self-certification defect three times (in `policy.json`, in
`classify_change.py`'s own `SELF_REFERENTIAL_PATHS` gap carried on #887, and here), and a single
stated invariant is what makes the fourth instance recognisable on sight.

**Co-location does not simplify this rule — it makes stating it more necessary.** §8.3 now puts
the exclusion list inside `policy.json`, so both inputs arrive from one base-resolved,
R3-floored file, and the obvious reading is that one file needs one resolution. It does not.
The direction is a property of how each field is *used*, not of where it is stored:

- Reading the whole file **from base only** loses §8.2's deliberate allowance for a PR that
  legitimately widens the R2 surface and wants its own new paths covered in the same run.
- Reading the whole file as **base ∪ head** would union the exclusions too — which is exactly
  the hole this section exists to close, reintroduced by a simplification that looks like
  tidying.

So the resolution is applied **per field, not per file**: union the `path_globs`, intersect the
exclusions, both against the same base. Co-location secures the *source*; the directional rule
governs how that source *combines*. Anyone later collapsing the two into "just read policy from
base" should read this paragraph first.

### 8.4.1 The input contract the invariant depends on: both sides of a rename

**The union in §8.2 is correct and was defeated by its input.** A set operation over changed
paths is only as complete as the path list handed to it, and that precondition was never
stated. Measured at `adcd6916dbd`:
`.github/workflows/gaussian_production_gates.yml:180` reduces the pull-request file list to
`files.map((file) => String(file.filename || ""))`, and `:194` does the same for the
`merge_group` `compareCommitsWithBasehead` path. **`filename` only — the string
`previous_filename` does not appear anywhere in that workflow.** So when an R2 file is *renamed*
to a path outside the R2 globs, the base policy is never handed the old path and cannot match
it; the derived lane skips production evidence for a file that was R2 until this very change.
Note what fails here: not the invariant, but an unstated precondition on its input.

**Requirement: the changed-path input includes both sides of a rename** — `filename` **and**
`previous_filename` — before any glob, union, or intersection is applied.

**This is not a new idea; it is an existing in-repo solution the derive design failed to
inherit.** `.github/workflows/baseline_qa.yml:402-407` already defines

```js
const pathsFromFile = (f) => {
  const out = [];
  if (f.filename) out.push(String(f.filename));
  if (f.previous_filename) out.push(String(f.previous_filename));
  return out;
};
```

applied with `flatMap` at `:416` and `:453`, and its comment (`:395-401`) names this exact
failure: a file moving from a watched prefix to an unwatched one is missed by matching
`filename` alone and *"we'd silently skip the visual gate."* The sibling workflow solved this;
the section prescribing a derivation for the other workflow did not go and look. #897 inherits
`pathsFromFile` rather than reinventing it.

**Second precondition: the list must be complete, not merely rename-aware.** Rename-awareness
fixes the paths that *are* returned; it does nothing about paths that are never returned at all.
The `merge_group` branch carries the workflow's own admission, at `:182-184`:

> `compareCommitsWithBasehead` caps at 300 files without pagination. Acceptable for this repo's
> typical PR size; revisit if merge-queue batches routinely exceed 300 changed files.

The call at `:188-193` passes `per_page: 100` and reads `compare.data.files` directly, with no
pagination and no truncation check, so a merge group above the cap hands the union a silently
truncated list. An omitted R2 path is not excused, not waived and not logged — it simply never
existed as far as the evidence decision is concerned.

**Requirement: paginate the merge-group comparison, and fail closed if the result is still
truncated** — both, not either. Pagination is the fix for the ordinary case and is what the
pull-request branch already does (`github.paginate`, `:174-179`); the fail-closed check is what
covers the case pagination cannot reach, since the compare API has its own hard ceiling
independent of paging. Choosing only pagination would leave a smaller version of the same hole
and no way to notice it; choosing only fail-closed would red the lane on batches that could
simply have been fetched. **A truncated input must never be silently treated as a complete one**
— on this input the safe direction is running the evidence lane, not skipping it, so if
completeness cannot be established the lane runs.

**The shape is worth naming, because it caught the same section twice.** Both preconditions —
rename-awareness and completeness — were invisible from inside the set operation. §8.2's union
and §8.4's intersection are correct set logic, and correct set logic over an input that is
missing members produces a confidently wrong answer with no symptom. **A set operation is only
as sound as the completeness of its input, and neither kind of incompleteness announces
itself.** Note also *how* the second one survived: the 300-file cap was **known, documented and
deliberately accepted** — for the purpose the workflow had when the comment was written. It
became a bypass when a new consumer arrived and inherited the input without re-reading the
limits attached to it. An accepted limit is scoped to the consumer that accepted it, and
inheriting the input inherits the limit whether or not the new consumer's author knows it
exists.

## 9. Members with no independent design content

Each of the following is a direct application of its finding's remediation direction. Listing
them explicitly is required by §1's closure rule: a member with no design section still has to
appear, so that the absence is a recorded judgement rather than an omission.

**T7c (#897) is no longer on this list.** It was, until §11 Q3 settled that it derives its path
filter from policy; that decision is design content, so T7c now has its own record at §8. A
member moving off this list is the expected outcome of an open question closing, and the move
is recorded here so the deletion is visible rather than silent.

- **T5 — release waiver expiry (#893).** No independent design content — direct application of
  `GS-AUDIT-TEST-005`'s remediation direction.
- **T6 — `validate_automation` wiring (#894).** No independent design content — direct
  application of `GS-AUDIT-TEST-004`'s remediation direction.
- **T7a — synthetic-fixture floor (#895).** No independent design content — direct application
  of `GS-AUDIT-TEST-009`'s remediation direction, at the *consumer*, per the finding's own
  recommendation. Its **scope** was open and is now settled (§11 Q1) at full scope, class R3;
  scope is not design, so it stays listed here.
- **T7b — orphan runtime probes (#896).** No independent design content — direct application of
  `GS-AUDIT-TEST-010`'s remediation direction.
- **T7d — StringName orphan guard (#898).** No independent design content — direct application
  of `GS-AUDIT-TEST-012`'s remediation direction.
- **T8a — metric provenance (#899).** No independent design content — direct application of
  `GS-AUDIT-SYS-011`'s remediation direction.
- **T8b — frame-completion tracker (#900).** No independent design content — direct application
  of `GS-AUDIT-GPU-017`'s remediation direction.
- **T9b — `nodes/README.md` truth (#902).** No independent design content — direct application
  of `GS-AUDIT-DOC-002`'s remediation direction.
- **T11a — stale fork claim (#904).** No independent design content — direct application of
  `GS-AUDIT-TEST-013`'s remediation direction.
- **T11b — dead shaders (#905).** No independent design content — direct application of
  `GS-AUDIT-BUILD-001`'s remediation direction.

### 9.1 Sequencing constraints (not design)

- **#896 before T3 step 2** (§4.6), so the fail-closed flip is not reasoning about scenarios
  that may be deleted.
- **#894 before the structural YAML port** that [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md)
  §5 defers to it. PyYAML is deliberately not a dependency of the required gate today; #894 is
  what makes it mandatory and fail-closed. #894's PR must record **which lane carries the
  dependency**, because a module-scope `import yaml` in `tests/agentic/` would `ImportError`
  at unittest *discovery* and red the only required check on every PR.
- **The wildcard's removal, §5.2's retag, and the three named declarations are one atomic
  change.** Removing `*][RequiresGPU]*` (count 59) before the #906/#907/#910 declarations exist
  leaves the manifest under-declared against a corpus that has not moved; adding them first
  double-declares the same cases. **And the retag must ride along**, because it is what takes the
  stranded population from 59 to 56: land it separately and the surviving wildcard declares 59
  against 56 stranded cases, which the guard fails for over-declaring; land the declarations
  without it and 56 does not add up. Every one of those intermediate states is a guard reporting
  on a tree that never existed. (Three declarations, not four and not five: #908 is fixed by the
  retag and #909 by the batch deletion — §5.4.) #909's batch deletion is the one piece that is
  *not* part of the atomic change: it touches `run_gpu_harness.py`'s `BATCHES` only, changes no
  case's stranded status, and can land on its own.

## 10. Known gaps and carried follow-ups

### 10.1 The gap this ADR opens and does not close

Policy's R3 machinery rule is a hand-written enumeration of five paths. **Enumerate-only is
the correct doctrine here** — a derived list would make the class follow the *filename*
instead of the *decision*: every new `tests/ci/` script would silently become R3, and renaming
a guard out of the pattern would silently drop it out of R3 with nothing to notice. The gap is
not the enumeration; it is that **nothing notices when a new `tests/ci/check_*.py` appears that is
neither listed nor explicitly exempted.** §7 states the self-registration rule for every guard
*this programme* creates, and the rule is binding — but it is binding on people, enforced by
review. Nothing in the tree fails when someone forgets it, and nothing at all constrains a guard
written outside this programme.

**Decision:** T9 is building a drift guard anyway. Fold the enumeration check into it **if
cheap** — for each `tests/ci/check_*.py` and `tests/ci/run_*.py`, require membership either in
policy's R3 machinery list or in an explicit, reasoned exempt list committed beside it, and
fail closed on a file in neither. If that is not cheap inside T9's diff, it is a **one-line
follow-up on #887**, not scope growth for any member of §1. Landing it is what turns §7's rule
from a convention into a guard, which is the difference this whole cluster is about.

**The guard fails in both directions, and this is not a new rule.** As first written, the check
above covers only *growth*: a guard file on disk that is listed nowhere. It misses the mirror —
**a policy enumeration entry naming a path that no longer exists**, which is what a rename or a
deletion leaves behind. The stale entry then sits in policy claiming R3 coverage for a file that
is gone, while the renamed file arrives unlisted; the growth half catches the second, nothing
catches the first, and the enumeration quietly drifts out of correspondence with the tree.

This is **§2.5 and §8.3's property 2 applied to a third site, not a new decision** — pin the
membership and fail on shrinkage as well as growth. The precedent is already running in this
repo: the quarantine manifest's `count` fails both ways, `actual > declared` as *"NEW stranded
case(s) joined an already-declared family"* and `actual < declared` demanding you *"lower
'count' … so the slack cannot be reoccupied"* (`check_test_lane_coverage.py:611-624`). The
consistency is the point: three sites, one rule, so a reviewer who has checked one knows what to
check in the others — and per §2.9 this is also the fourth-plus place where a rename is the
thing that breaks it.

### 10.2 Carried by reference — open, not re-opened here

From **#886**, tracked on **#887**:

- `scripts/agentic/classify_change.py` is **not** in `SELF_REFERENTIAL_PATHS` (verified: the
  tuple holds `.agentic/policy.json` only), so a PR rewriting the classifier publishes a class
  derived by its own code. Published-not-enforced surface; the accepted remedy is the one-entry
  addition.
- The **`JOB` key vs `name:` value** gap: the workflow guard matches the job *key*, while the
  required-check context is the `name:` value, so renaming only `name:` bricks the repo with
  every test green.
- The **`shell_text()` expression residual**: GitHub expression spans are stripped before
  matching, so a `||` *delivered into* the shell by an expression would be invisible to the
  default-deny rule.

From **PR #911**, tracked on **#890** (T2 — not a member of §1):

- A **whole-file rename** of a baselined test file produces a **false-RED**. Safe direction —
  it blocks a legitimate change and licenses nothing — but it is a known cost, with git rename
  detection named as the likely remedy.
- The **regenerate path compares against the flattened baseline** (all fingerprints, any key),
  a deliberate loosening disclosed in the PR: a fingerprint that *moves between keys* is not
  seen as an addition. Related, from the same PR: `--regenerate-null-deref-baseline` has no
  coverage beyond the single refusal test added there.
- **`_blob_at_base`'s `ls-tree` pre-check is unpinned** — deleting it leaves every test green
  (measured twice, rounds 2 and 3).

## 11. Decision record — five questions that were open, and how they were settled

These five were **genuinely open** when this ADR was first filed: each was a fork the author
could see but had no standing to close, and each was written up as an unanswered question with
both branches argued rather than a recommendation dressed as an analysis. **All five have since
been settled by the maintainer, and each decision is now folded into its owning section.** This
section is kept — rather than deleted once the answers landed — because the reasoning is the
part that is worth anything later: a reader six months out needs to know *why* #895 is R3, not
merely that it is, and a decision with its alternatives erased is indistinguishable from a
decision nobody made.

Two of the five (Q1, Q5) were settled by the programme principle stated in the preamble:
**class follows the right design; design never follows class.**

**Q1 — #895's scope, and therefore its class. Settled: FULL SCOPE, R3 accepted.**

*What was open.* `GS-AUDIT-TEST-009` puts the fixture-count floor "at load in
`run_runtime_validation`". Measured at `adcd6916dbd`:

```
$ python scripts/agentic/classify_change.py --paths \
      tests/runtime/prepare_synthetic_assets.py tests/runtime/check_benchmark_asset_paths.py
risk_class: R1
$ python scripts/agentic/classify_change.py --paths \
      tests/runtime/prepare_synthetic_assets.py tests/runtime/check_benchmark_asset_paths.py \
      tests/runtime/run_runtime_validation.py
risk_class: R3
  R3  tests/runtime/run_runtime_validation.py  (CI deterministic-check / release-gate machinery)
```

The producer-side-only scope grades R1 and is cheaper; the consumer-side scope grades R3 and is
the finding's own recommendation.

*Settled.* **Enforcement goes at the consumer, `run_runtime_validation`, as
`GS-AUDIT-TEST-009` recommends. The R3 class is accepted, not worked around.** Producer-side
enforcement guards the paths that *build* fixtures; it leaves the manifest default-open at the
point of *load*, which is precisely the hole that produced **#669** — a missing fixture yielding
0 splats, ~2400 FPS, and a passing benchmark. A floor that the loader does not check is a floor
that a stale import cache, a hand-edited manifest or a new load path walks straight past. R1
here would have bought a cheaper review by declining to guard the place the defect actually
occurred.

*Recorded in.* §1's row (now **R3**); §9's T7a entry, which keeps T7a on the no-design list
because scope is not design. Q1 is the clearest instance of the programme principle: the R1
option was available, defensible on paper, and wrong.

**Q2 — the `issue_url` for T10's expected-fail entries. Settled: GPU-001 gets its own issue, at
T10 time.**

*What was open.* `GS-AUDIT-GPU-001` has no GitHub issue — Phase-2/3 findings were deliberately
excluded from the Phase-1 batch — while the manifest fields require a real target. Either
GPU-001 is filed, or the entries point at #903 and are re-pointed later.

*Settled.* **The entries point at GPU-001's own issue, which T10 files when it is implemented.
Not #903, and not filed now.** Full reasoning in §6.4: filing it is *within* the Phase-1
exclusion policy rather than an exception to it, because an in-repo artifact referencing the
defect makes it active work; and #903 cannot serve, because **#903 closes when the gate covers
production defaults while the oracles flip to blocking when the defect is fixed** — two events,
two trackers.

*Recorded in.* §6.4 (new), and §6.5's flip trigger.

**Q3 — whether T7c derives `renderStreamingPrefixes` from policy's R2 globs. Settled: derive,
with an explicit reasoned exclusion list.**

*What was open.* `GS-AUDIT-TEST-011` raises deriving as an option; the ADR's first revision did
not take it, and flagged that taking it would be design content that could not be decided inside
#897's PR.

*Settled.* **Derive.** The rationale is `evidence-integrity.md` practice 5 applied to its own
test case — when a new `streaming_*.cpp` appears, the prefix list is **stale**, not awaiting a
decision, because policy already decided that R2 paths need GPU evidence. Measured drift at
`adcd6916dbd`: **45 of 197** R2 files are invisible to the evidence lane, 32 of them in `core/`
alone. **Required addition: an explicit exclusion list carrying a reason per entry**, so an R2
path that genuinely does not need the lane can be exempted *visibly* rather than by quietly
narrowing an R2 glob in `policy.json` — which would be the programme principle inverted, moving
a file's risk class to buy a cheaper CI run.

*Recorded in.* §8 (new, T7c's own design section); removed from §9's list, with the move noted
there. This is the amendment §11 Q3 said would be needed — made here as a reviewed document
change, exactly as the closure rule requires.

**Q4 — whether T3's fail-closed flip is in this cluster or in Phase 2. Settled: in-cluster, on
an evidence-based trigger.**

*What was open.* §4.6 placed step 2 in the cluster but did not say what fires it; if the arming
ratchet needed GPU-runner numbers, step 2 might have to become Phase-2 work.

*Settled.* **In-cluster, triggered by a defined soak — not a date and not a phase boundary.**
Advisory until N consecutive runs show the marker present across the profile set, then the flip
PR lands *citing the soak evidence*. Deferring to "Phase 2" would make the flip somebody's
forgotten TODO, which is the deferred-enforcement rot the audit catalogued: an advisory lane
with no arming condition never arms. And explicitly: **if the soak exposes marker gaps, that is
a finding to fix, not a reason to stay advisory silently.**

*Recorded in.* §4.6 step 2, including a proposed N and profile set that the flip PR must
confirm against real runs rather than inherit.

*Two corrections found in review, both kept visible in §4.6 rather than silently patched.* The
first draft of the trigger was **unsatisfiable** — it demanded a marker from every registered
scenario in every run, and no CI profile selects more than 10 of the 11 — so the "evidence-based
trigger" would have armed nothing, which is the same forgotten-TODO outcome this answer was
chosen to avoid, reached by the opposite route. It is now two obligations: per-profile
completeness, plus a separately guarded union-coverage property. The second: the soak's evidence
is currently **destroyed before it is read** — both `gaussian_production_gates.yml` invocations
write the same default report path and the later one overwrites the earlier — so distinct
`--report-path` values and preserved artifacts are now a stated precondition, and T3
implementation work. Neither correction changes the answer to Q4; both change whether the answer
could have been carried out.

**Q5 — whether T10 reaches `baseline_qa.yml`. Settled: not pre-decided — and the design then
made T10 R3 anyway.**

*What was open.* On the minimal scope — QA scenes plus `qa_test_runner.gd` — T10 measures R1,
not the R3 its task plan carried; it reaches R3 only if the workflow is edited.

*Settled.* **The class is deliberately left to follow the design, and is not pinned in advance
either way.** The question "does T10 reach `baseline_qa.yml`?" is answered by whether the
correct design needs the workflow edited — for instance to surface the expected-fail count on
the job — and not by which answer is cheaper to review. Pre-deciding "keep it R1" would be a
scope decision made for a class reason, which is the move the programme principle forbids;
pre-deciding "make it R3" would be the same error with the sign flipped, buying ceremony
instead of avoiding it. §6 covers T10's design **either way**, so the ADR requirement is
satisfied whichever class the finished design measures.

*And then the design answered it.* Specifying the second clause of `TEST-008`'s remediation
direction — the override-diff guard, §6.6 — introduces a new `tests/ci/check_*.py`, which
self-registers into policy per §7 and floors T10's PR at **R3** (measured in §6.6). So T10 is
R3, arrived at the way the principle requires: nobody chose the class, the class fell out of
writing down the whole fix. The original question stands open as a *design* question — whether
the finished design also wants `baseline_qa.yml` edited, for instance to surface the
expected-fail count on the job — but it no longer decides anything about the class, because the
guard already floored it.

Worth stating plainly, since it is the principle's sharpest test in this cluster: the cheaper
reading was available right up to the end. Leaving §6 at the scenes alone would have kept T10 at
R1 and looked like a defensible minimal scope — and it would have shipped **half** of
`TEST-008`'s remediation while the finding's own root cause ("nothing diffs QA-project settings
against manifest/GLOBAL_DEF defaults") went unaddressed. Trimming a fix until it grades R1 is
the failure this ADR is about, one level up.

*Recorded in.* §1's T10 row (now **R3**), §6.6, and §7's instance list.

## 12. What would change this decision

- **A second reviewer.** A CODEOWNER or second human who is not the author makes §3's waiver
  unnecessary; the disposition paragraph shrinks to facts with no waiver in it, and the
  rubber-stamp risk this ADR is guarding against disappears with it.
- **GPU-001 being fixed.** §6's expected-fail scenes become ordinary blocking scenes and the
  `EXPECTED_FAIL_SCENES` mechanism loses its reason to exist. It should then be **deleted**,
  not kept in case something else needs it — a general-purpose expected-fail facility is a
  general-purpose place to park red.
- **A member leaving the cluster, or a new one wanting in.** Either is an amendment to §1's
  table, reviewed as a document change. This ADR does not cover R3 work it does not name, and
  that is the property that makes the single-record economy defensible.
