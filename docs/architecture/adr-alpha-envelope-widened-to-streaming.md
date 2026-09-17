# ADR: Widen the public-alpha envelope to streaming open worlds rather than narrow its gate (#1016)

- **Status:** Accepted (maintainer decision, 2026-09-17). Supersedes the scope
  sentence in `docs/governance/release-acceptance-bar.md` §10.1 that placed streaming
  open worlds outside the alpha envelope.
- **Risk class:** this document is R0 (`docs/**`), as is the acceptance-bar change that
  lands with it. It records a **product-scope** decision, not a code change. No manifest,
  workflow or renderer file is edited by this ADR or by its sibling bar change — the whole
  point of the decision is that `docs/reference/renderer_release_gate_manifest.json` is
  left exactly as it is.
- **Tracking:** #1016. **Gates:** #961 (candidate bundle generator) must not be built
  until this is decided, because it determines what the bundle must contain.
  **Related:** #960 (the gate has never executed), #963 (hand-written issue ledger),
  #964 (manifest `status: "foundation"` has no exit criteria).
- **Verified against:** `origin/master` = **`b915afc51c5`**. Every file:line below was
  read at that commit, and §2 records the gate run that replaced #1016's read-derived
  claim with an executed one.

## 1. Context

`docs/reference/renderer_release_gate_manifest.json` is the only gate a public-alpha tag
can pass through. It requires, for a candidate:

- ten artifact groups (`artifact_requirements.required_groups`, `:374-385`), one of which
  is **`open_world_proof`**;
- six benchmark lanes (`benchmark_acceptance.candidate_required_lanes`, `:344-351`), two
  of which are **`streaming_corridor`** and **`city_flyover`**.

`docs/governance/release-acceptance-bar.md` §10.1 put streaming open worlds *outside* the
alpha envelope (`:223-224`: "Streaming *open worlds* remain out — admitting the world node
is not admitting the 50M chunked ladder"), and §11 listed the 50M open-world asset as a
**v1.0** item (`:252`).

There is no second publishing route. `public_alpha_predicate.not_satisfied_by` rules out
`nightly`, `stable`, `manual workflow intent` and `advisory evidence` (`:14-19`), and
`disallow_manual_downgrade: true` (`:22`) forecloses adding a channel that skips the gate.
`disallow_open_world_advisory_only: true` (`:24`) specifically forbids satisfying the
open-world requirement with an advisory signal.

So the alpha could not pass its own gate, and no amount of work on the alpha's declared
scope would have changed that.

## 2. The premise was re-verified by running the gate, not by reading it

#1016 was explicit that it had been found by reading the manifest against the bar, and that
the gate has never executed (#960). A decision this size should not rest on a reading, so
the gate was run before this ADR was written.

**Method.** A candidate evidence bundle was synthesised against the **real** manifest
(`docs/reference/renderer_release_gate_manifest.json`, not a test fixture) and passed to
`python tests/ci/check_renderer_release_gates.py --mode candidate`, twice: once containing
all ten artifact groups and all six lanes, once with exactly `open_world_proof`,
`streaming_corridor` and `city_flyover` removed. Artifacts were real files inside the
repo with correct SHA-256 digests, commit and mtime metadata, so integrity validation
passed rather than short-circuiting.

**Result.**

| Run | Bundle | Exit | Failures |
| --- | --- | --- | --- |
| B | all ten groups, all six lanes | **0** | none — `Renderer release gate candidate check passed` |
| A | the three removed | 1 | **exactly three** |

Run A's complete failure list:

```text
candidate artifact group missing: open_world_proof
candidate benchmark lane missing: streaming_corridor
candidate benchmark lane missing: city_flyover
```

This is a discrimination test, not an absence-of-signal one: run B proves a bundle of this
shape *can* reach a pass, so run A's three failures are the only thing standing between an
otherwise-complete alpha candidate and a green gate. The enforcement points are
`_validate_candidate_artifacts` (`tests/ci/check_renderer_release_gates.py:1150-1152`) and
the lane loop at `:1720-1724`.

**A note on why nothing had caught this.** The gate's own test suite builds a stub manifest
with `"required_groups": ["linux_release_archive", "known_limitations_page"]` and
`"candidate_required_lanes": ["static_baseline"]`
(`tests/ci/test_renderer_release_gates.py:197` and `:193`). The real manifest's groups and
lanes are exercised by no test, which is why a contradiction this large survived in a file
that is otherwise heavily guarded.

#1016's premise therefore holds, and is now executed rather than inferred.

## 3. Options

1. **Scope the candidate requirements by channel** — give `artifact_requirements` and
   `benchmark_acceptance` a public-alpha profile that drops the three, and keep the current
   set as the v1.0 profile. This is #1016's own option 1.
2. **Widen the alpha envelope to include streaming open worlds, and keep one profile.**
   *(Chosen.)*

## 4. Decision

**The public alpha's envelope widens to include streaming open worlds. The gate is kept
exactly as written.**

The alpha's supported envelope, previously "a single resident scene" plus the world node,
now also covers the streaming open-world route. Because the envelope and the gate are now
in agreement, `renderer_release_gate_manifest.json` needs no change, and in particular no
channel profile, no waiver, no threshold edit and no advisory downgrade.

`docs/governance/release-acceptance-bar.md` §10.1 is amended to say this; §5, §10 and §11
are amended where the widening contradicts them. Those edits land with this ADR.

### 4.1 What the alpha now promises

- Forward+, single view (unchanged — the pre-upscale composite is single-view only).
- `GaussianSplatNode3D` with an imported asset (unchanged).
- `GaussianSplatWorld3D` (unchanged — already admitted by the §10.1 decision that made
  #862 an alpha blocker).
- **New: the streaming open-world route**, including chunked content and the camera motion
  that drives chunk turnover and boundary crossing.

### 4.2 What the alpha still excludes

The widening is to streaming, and to nothing else. Still outside the alpha envelope:

- **Multiview and reflection probes**, which keep the legacy post-scene hook, and **Forward
  Mobile**, which is a separate route.
- **Multi-node scenes.** The §10 table's v1.0 column reads "+ multi-node + streaming"; this
  decision moves `streaming` and leaves `multi-node` where it is. #842 (no benchmark lane
  varies node count) stays a v1.0 item.
- **A performance promise on the streaming route.** The alpha's bar is user-visible
  correctness (§10); the peer comparison in §3 remains v1.0-only.

### 4.3 Three evidence lanes must now run and pass before a stable tag

Stated plainly, because it is the whole cost of this decision: a `v*` tag cannot publish
until **all three** of the following exist and pass in the release run. Until this ADR they
were required by the gate and disowned by the bar; now they are owned.

| # | Requirement | Kind | Produced by |
| --- | --- | --- | --- |
| 1 | `open_world_proof` | artifact group | `lane_open_world_corridor_proof.tscn` / `benchmark_open_world_proof_lane.gd` |
| 2 | `streaming_corridor` | benchmark lane | `lane_streaming_corridor.tscn` |
| 3 | `city_flyover` | benchmark lane | `lane_city_flyover.tscn` |

All three lanes exist in the benchmark suite today. None of them is release evidence today —
§5 is about the distance between those two statements.

## 5. What this costs

This section exists because the decision is cheap to write and expensive to honour, and the
cheap half must not be recorded without the expensive half.

### 5.1 Two of the three lanes do not measure what the widened promise claims

`tests/fixtures/benchmark_asset_manifest.json`, `lane_metadata`, records each lane's asset
classification and says in its own `notes` what the lane is and is not:

| Lane | `asset_classification` | The manifest's own note |
| --- | --- | --- |
| `streaming_corridor` | `lightweight_smoke` | "currently backed by test_splats.ply; useful for proof-shape smoke coverage, **not representative chunked evidence**" |
| `city_flyover` | `lightweight_smoke` | "currently backed by test_splats.ply; … **not representative chunked evidence**" |
| `open_world_corridor_proof` | `chunked_open_world_candidate` | "honest large-world candidate evidence, but **not yet promoted real_chunked proof**" |

`docs/testing/benchmark-suite.md:306-310` states the rule directly: "No currently documented
benchmark lane should be cited as representative chunked streaming evidence unless its
manifest classification is upgraded to `real_chunked`." All three lanes are listed
`Suite-only` there (`:298-300`), and `docs/performance/index.md:189-191` lists all three as
"Defined in the benchmark suite, not yet published".

**The consequence is the sharpest thing in this ADR.** Satisfying the gate with these three
lanes *as they are classified today* would produce a green gate over evidence the repository's
own manifest says is not representative of the capability the alpha would then be promising.
That is precisely the defect shape bar §4.2 makes blocking — "a benchmark that measures less
than it names" — and precisely what `disallow_open_world_advisory_only: true` exists to
forbid.

**Therefore the widening carries an obligation, not just a scope change:** before the alpha
tag, either

- the asset classification backing `open_world_proof` is promoted to `real_chunked` and the
  two support lanes are re-backed by content that justifies the promise; **or**
- the alpha's streaming promise is explicitly bounded in the bar and in the release notes to
  what the lanes actually exercise, and the word "open world" is not used unqualified.

Which of the two is a maintainer decision and is **not** taken here. What is decided here is
that the gate is not satisfied by passing three lanes that disclaim their own evidentiary
weight.

### 5.2 Streaming has no passing QA-scene coverage at all

Every streaming QA scene in the project is quarantined
(`tests/examples/godot/test_project/scripts/qa_test_runner.gd:51-84`):

| Scene | Quarantine reason, verbatim |
| --- | --- |
| `qa_stream_visual_smoke.tscn` | "#786: streaming/visual readiness is never reached (luma variance 0.00009 vs the 0.0002 gate) reproducibly across runs." |
| `qa_stream_multi_asset.tscn` | "disabled until the runtime surface can prove true resident/streaming coexistence." |
| `qa_stream_chunk_loading.tscn` | "streaming monitors not populated." |
| `qa_stream_eviction_churn.tscn` | "streaming monitors not populated." |

These are QA scenes, not the three benchmark lanes, so they do not block the gate directly.
They are recorded because the alpha would now be promising a capability whose entire
QA-scene coverage is switched off — including, in two cases, because the monitors that would
observe it are not populated.

### 5.3 Five v1.0 blockers become alpha blockers

Bar §4.1 makes any defect reachable inside the supported envelope a blocker. Widening the
envelope therefore moves work, and this is mechanical rather than a judgement:

| Issue | State | What it is | Was | Now |
| --- | --- | --- | --- | --- |
| #318, #320 | OPEN | streaming performance | v1.0 | **alpha** |
| #786 | OPEN | `qa_stream_visual_smoke` never reaches visual readiness on a real GPU | v1.0 | **alpha** |
| #883 | OPEN | GPU streaming stress `frame_p95_to_avg_ratio` 3.69 on an idle runner | v1.0 | **alpha** |
| (no issue) | — | the 50M chunked open-world asset must become a passing lane, not a contract (bar `:252`) | v1.0 | **alpha** |

#842 (node-count benchmark) is **not** moved: it is multi-node, which §4.2 leaves outside
the alpha.

### 5.4 The real-scan visual pass grows

Bar §6 requires the human visual pass "across the supported envelope", and §10 requires it at
both stages. Widening the envelope widens the pass: it must now include the streaming route.
The procedure drafted on #1012 covers six configurations on the resident and world routes and
does not cover streaming; it needs a streaming configuration added before it is executed.

### 5.5 What it does not cost

The `deferred_requires_gpu_waivers` expiry of **2026-10-31** is unchanged by this decision —
it was already binding. It is noted only so nobody attributes it to the widening.

## 6. Why the gate was kept rather than narrowed

Narrowing (option 1) was the option #1016 itself leaned toward, and it is defensible. It was
not chosen, for four reasons.

1. **The gate's requirements were not arbitrary.** `open_world_proof`, `streaming_corridor`
   and `city_flyover` were put in `required_groups` and `candidate_required_lanes`
   deliberately, alongside `disallow_open_world_advisory_only: true` — a flag whose only
   purpose is to stop exactly the kind of scoping-down that option 1 performs. Removing the
   requirement and keeping the flag that guards it would be incoherent; removing both would
   discard a guard to make a check pass, which `AGENTS.md` forbids.
2. **A profile mechanism is new machinery on the critical path.** Option 1 means adding
   channel-scoped profiles to the manifest, teaching `check_renderer_release_gates.py` to
   select between them, and testing that selection — R3 work, on the one file whose job is to
   be hard to weaken, at the moment it has never once executed (#960). The failure mode of a
   mis-specified profile is a gate that passes when it should not, which is worse than a gate
   that demands too much.
3. **The alpha's purpose argues for the wider scope.** Bar §10 states it: "The alpha exists
   so that design errors are found early by users rather than late by us." Streaming is where
   this renderer's unproven design decisions are concentrated — §5.2's four quarantined
   scenes are that in one table. An alpha that excludes streaming gets early feedback on the
   part already best covered and none on the part that needs it.
4. **It is honest about the ship date rather than about the scope.** Option 1 would let the
   alpha tag sooner by promising less. That is legitimate, but the maintainer's judgement is
   that a GodotGS alpha which cannot stream is not the product being built, and that moving
   the date is better than moving the definition.

**The honest counterpoint, recorded:** this decision makes the alpha strictly harder to reach,
by an amount §5 does not fully quantify — #318 and #320 are open performance issues with no
estimate attached, and §5.1's classification question could turn into the 50M asset programme
in full. If that proves out of proportion, the right response is to revisit *this* ADR in the
open, not to quietly re-narrow the gate or mark a lane advisory.

## 7. Consequences

### 7.1 Landing with this ADR

- `docs/governance/release-acceptance-bar.md`: §5, §8.1, §10 and §10.1 amended so the bar and
  the manifest agree, plus an explicit specification of the candidate bundle (§9.1) so #961
  has something to build against rather than a guess.

### 7.2 Work this creates or re-prioritises

| Item | Owner issue | Note |
| --- | --- | --- |
| Decide §5.1: promote the asset classification, or bound the promise in words | **#1016** (keep open until decided) | Blocks the tag, not this ADR |
| Candidate bundle generator | #961 | Unblocked by this ADR; build against all ten groups and all six lanes |
| Dry-run the candidate gate in CI | #960 | Should still land first; §2 is a local run, not a CI one |
| Streaming configuration added to the visual-pass procedure | #1012 | §5.4 |
| Streaming blockers re-labelled from v1.0 to alpha | #318, #320, #786, #883 | §5.3 |
| Un-quarantine the streaming QA scenes, or record them as accepted alpha limitations | #786 + the three untracked reasons | §5.2 |

### 7.3 The `alpha-relevant` label is still inert

`public_alpha_predicate.required_issue_query.alpha_relevant_p1_labels_all` requires the labels
`priority:P1` **and** `alpha-relevant` (`:35-38`). **`alpha-relevant` does not exist in this
repository** — `gh label list` returns 40 labels and none is it (checked 2026-09-17). The
criterion can therefore never match, which is bar §4.2's "a criterion that cannot fail is not
a criterion".

This ADR does **not** fix it, and deliberately so: the fix is either creating and applying the
label across the open P1 set or deleting the field, and both are the issue-ledger question
#963 owns. It is recorded here because #1016 raised it and because this ADR must not be read
as having resolved every finding on that issue. Widening the envelope makes it more pressing,
not less — there are more in-envelope P1s now than there were.

## 8. What was not verified

- **Neither the gate nor any lane was run on a GPU for this ADR.** §2 is a local, synthetic
  run of the validator against the real manifest; it proves what the gate demands, not that
  any lane can satisfy it. No claim here says a lane passes.
- **§5.1's classifications are read from the manifest, not re-derived** from the assets. The
  claim is that the repository classifies them that way and says so, which is what makes
  shipping them as proof incoherent; whether a different judgement of the same assets is
  defensible is the §5.1 decision itself.
- **The 2.1 s resubmit cost, the #883 ratio and the #786 luma variance** are quoted from their
  issues and from the quarantine entry, not re-measured.
