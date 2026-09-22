# ADR: The candidate gate must distinguish measured evidence from defaulted evidence (#1016, #961)

- **Status:** Proposed (2026-09-22).
- **Risk class:** **R3** — it changes `docs/reference/renderer_release_gate_manifest.json` and
  `tests/ci/check_renderer_release_gates.py`, both listed R3 in `.agentic/policy.json`.
- **Tracking:** #1016 (the ADR-widening decision that created the obligation), #961 (the
  candidate bundle that must satisfy it). **Related:** #960 (the gate has never executed).
- **Verified against:** `origin/master` = **`6f4552076c7`**. Every file:line below was read at
  that commit, and every result marked **[ran]** was executed against it.
- **Supersedes nothing.** It implements the §4.3 obligation that
  [`adr-alpha-envelope-widened-to-streaming.md`](adr-alpha-envelope-widened-to-streaming.md)
  recorded and explicitly declined to discharge.

## 1. Context

The #1016 widening put the streaming open-world route inside the public-alpha envelope and kept
the gate exactly as written. Its §4.3 recorded that the open-world obligation is **human-enforced,
not machine-enforced**, and named the two ways to close it: a content validator for the
`open_world_proof` group, or promoting `open_world_corridor_proof` into
`candidate_required_lanes`. It described both as R3 work and did neither.

This ADR does the first, and records a second hole found while verifying the first.

## 2. The two holes, both executed rather than inferred

### 2.1 `open_world_proof` is integrity-only

`_validate_candidate_artifact_group` (`tests/ci/check_renderer_release_gates.py:1050-1065` at the
base commit) called exactly four checks — required fields, integrity, commit, mtime. No validator
inspected any artifact's content.

**[ran]** A candidate bundle whose ten artifact groups all pointed at `README.md` with correct
SHA-256 digests, against the **real** manifest:

```text
Renderer release gate check failed:
 - candidate evidence missing gpu_harness_report
 - candidate evidence missing benchmark_report
 - candidate issue snapshot missing manifest-tracked blocking issue #351; ...
 - candidate issue snapshot missing manifest-tracked blocking issue #352; ...
 - candidate issue snapshot missing manifest-tracked blocking issue #360; ...
```

Five failures, **none about any artifact group**. The remaining failures are the parts the probe
deliberately omitted. This is a discrimination result, not an absence-of-signal one: pointing the
same group at a `.gd` source file behaved identically, and zeroing its digest produced exactly one
failure, so the integrity check is live — it is simply checking the wrong property.

### 2.2 The streaming lane fields are structural constants

This one is **not** in #1016 and is the more dangerous of the two, because it survives the fix to
§2.1.

`benchmark_acceptance.required_fields_non_null` (manifest `:352-372`) demands `queue_pressure` and
`proof_status` be non-null. On both required streaming lanes they are non-null and neither is a
measurement:

- `modules/gaussian_splatting/renderer/render_diagnostics_orchestrator.cpp:615-625` — the `else`
  branch taken whenever `perf.streaming_state` is empty writes
  `r_metrics["streaming_queue_pressure_frames"] = static_cast<int64_t>(0)`. A lane with no
  streaming system reports zero queue pressure, indistinguishable from a streaming run that had
  none.
- `tests/runtime/run_benchmark.py:508` short-circuits `proof_status` to `"not_applicable"` for any
  lane without a contract, and `LARGE_WORLD_PROOF_CONTRACTS` (`:382-408`) contains only
  `open_world_corridor_proof`. So `streaming_corridor` and `city_flyover` always report
  `proof_status: "not_applicable"`, `proof_valid: true`.

A third, smaller finding sits beside them: **`queue_pressure` is not a key the harness emits at
all.** Real rows carry `streaming_queue_pressure_frames`
(`docs/assets/data/benchmark_suite_report.json`, `city_flyover` row); the manifest asks for
`queue_pressure`. The ADR-widening's Run B passed because its rows were hand-synthesised to the
manifest's names, so the mismatch has never been exercised.

This is [evidence integrity](../governance/evidence-integrity.md)'s **"fail-open on absence"** and
**"summarising across a boundary the data does not cross"** in one place.

## 3. Decision

**Required evidence must be validated for what it says, and "not measured" must never be
encodable as a passing value.**

Two changes, landed as two PRs against this one ADR.

### 3.1 Content-validate declared artifact groups (this PR)

`artifact_requirements.content_validators` is a new manifest block mapping a group name to a
validator specification. `_validate_candidate_artifact_content` dispatches on a `kind` drawn from a
**closed** set, `_CONTENT_VALIDATOR_KINDS`.

For `open_world_proof` the validator requires the artifact to be the corridor-proof lane's own
report and to show telemetry that was measured:

| Assertion | Rejects |
| --- | --- |
| parses as a JSON object | `README.md`, a `.gd` file, any non-JSON artifact |
| `lane_id == "open_world_corridor_proof"` | a report from a lane that streams nothing |
| a `proof_metrics` block exists | a JSON file that is not a lane report |
| every flag in `required_telemetry_available` is `true` | numbers that were defaulted rather than measured |
| every `minimum_values` entry is a number at or above its floor | a single-chunk run, an empty measurement window, a `null` read as zero |

Three deliberate fail-closed choices:

1. **An unknown `kind` is a failure**, so a manifest typo cannot silently validate nothing.
2. **A validator configured for a group that is not in `required_groups` is a failure**, because
   it would never run — the "guard wired to nothing" shape.
3. **Content validation is skipped only when integrity already failed for that group**, so "the
   file could not be read" can never become the reason a proof group goes unchecked.

Why enumerate rather than derive: per evidence-integrity practice 5, this list *decides* policy.
Discovering a new artifact group must mean somebody chooses whether it needs content validation,
not that the corpus silently redefines the contract.

### 3.2 Make "not measured" unencodable as a pass (sibling PR)

The source already distinguishes the two states and the benchmark pipeline discards the
distinction: the same `else` branch that writes the zero also writes
`streaming_diagnostics_category = "unknown"` and `streaming_diagnostics_reason = "unavailable"`
(`render_diagnostics_orchestrator.cpp:616-618`), and neither key is in the lane report's
whitelist (`benchmark_suite_lane.gd:91-171`). The fix therefore restores an existing
source-of-truth signal rather than inventing a parallel one, and needs **no engine change**.

`run_benchmark.py` then fails closed on it, exactly as it already does for `route_uid` (`:1769-1771`)
and `stage_statuses` (`:1824-1831`) — a pattern this repository adopted for #351 E3 after Codex
#418. The existing `required_fields_non_null` machinery does the rest: an unmeasured field becomes
`null` and the candidate gate rejects it with no manifest threshold change.

## 4. Consequences

- **The gate gets stricter and nothing in CI breaks**, because the candidate gate has never
  executed in CI (#960) and no workflow produces a bundle today.
- **A candidate bundle can no longer be satisfied by the two smoke lanes as they are backed
  today.** That is the intended effect and is exactly what #1016 §5.1 says the gate must not
  accept. It does not make the alpha reachable; it makes an unreachable alpha *visible*, which is
  the difference between a gate and a formality.
- **`open_world_proof` now has a defined producer**: the `open_world_corridor_proof` lane's report
  JSON, written by `benchmark_suite_lane.gd:1568-1572`. Before this ADR the group had no producer
  in any shape the gate would accept.
- **The obligation is not fully discharged.** A validator can only assert what the report claims.
  It cannot verify the report came from a real GPU run; that binding is the commit/mtime machinery
  and the human signer, and it remains as weak as it was.

## 5. Alternatives considered

1. **Promote `open_world_corridor_proof` into `candidate_required_lanes`** — #1016 §4.3's other
   option. It is complementary, not alternative, and it is the plan's step 7. It is *not* done here
   because the lane has never run (see §6), so adding it to the required set would make the gate
   demand evidence nothing can currently produce, before the machinery that produces it exists.
2. **Hash-pin the proof artifact to a known-good file.** Rejected: it pins bytes, not properties,
   and would have to be re-pinned on every run, which is the "compared against itself" shape.
3. **Patch the reader to treat `0` as suspicious.** Rejected explicitly. A reader-side heuristic
   cannot distinguish a real zero from a defaulted one; only the producer knows. §3.2 keeps the
   distinction at the source.

## 6. What was not verified

- **No lane was run and no GPU was used for this ADR.** Every `[ran]` result is a CPU-only
  invocation of the existing checker. **No claim here says any lane passes**, and the
  honest-report fixture in `OpenWorldProofContentValidationTests` is *shaped* like a real report
  but is **not a recorded run**; it exists to prove the validator's legal route still works
  (evidence-integrity practice 6).
- **`open_world_corridor_proof` has never executed in CI.** Its only step
  (`gaussian_production_gates.yml:611-617`) is gated on `workflow_dispatch` plus an input, and
  every `workflow_dispatch` run of that workflow is from March 2026. In the 2026-09-21 scheduled
  run the step is `skipped`. That is tracked separately and is the plan's step 4.
- **The 20M corridor world these criteria describe does not exist on disk.** The stage manifests
  are `staging_status: "planned_unstaged"` and the lane synthesises the world at runtime. Whether
  that content earns a `real_chunked` classification is the open §5.1 maintainer decision on
  #1016; **this ADR promotes no classification.**
