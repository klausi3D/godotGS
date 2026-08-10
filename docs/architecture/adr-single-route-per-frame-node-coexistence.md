# ADR: `GaussianSplatWorld3D` + `GaussianSplatNode3D` in one scene — warn now, route later (#788)

- **Status:** Accepted. **Option 1 (editor configuration warning) is accepted as the
  interim** and is specified implementably in §6 below. **Option 2 (genuine per-submission
  routing) is accepted as the target direction and deferred** — it needs its own ADR and
  its own evidence. **Option 3 (hard-reject the combination) is rejected**; §5.3 records
  why, including the cost it would impose on scenes that already exist.
- **Risk class:** this document is R0 (`docs/**`). The Option 1 implementation it specifies
  lands in `modules/gaussian_splatting/nodes/**` and `modules/gaussian_splatting/tests/**`,
  which `.agentic/policy.json` classifies as **R1** — no ADR is *required* there, so this
  record exists because the decision is a product-behaviour decision, not because a gate
  demanded it. Option 2 will touch `modules/gaussian_splatting/renderer/**` and
  `core/*streaming*` (**R2**) and must not be started against this ADR alone.
- **Prerequisite for implementing Option 1: #862 must land first.** This is a hard
  dependency, not a caveat, and it is stated here rather than in a note because anyone
  reading this ADR in order to *write* the warning has to hit it before they write code.
  §6.1's conjunct P reads the *assigned* resource while the director keeps the *apply-time*
  payload `Ref`, and the two diverge in **both** directions: adding a payload gives a false
  positive (§6.1), and **removing one gives a false negative — the warning goes silent
  exactly while the stale submission is still steering the route** (§6.1, T13). A diagnostic
  that goes quiet when the problem is real is worse than no diagnostic, and no wording change
  fixes it. The alternative — basing P on the submitted payload — is rejected in §6.1 (it is a
  live director query and would make the warning blink). **#863 is not a hard prerequisite**,
  but it is what forces the §6.2 workaround to carry an extra manual step; the evidence and
  the distinction are in §5.1.
- **Tracking:** #788 (this decision). **Split from:** #785 (`qa_visual_diff` /
  `qa_sh_rotation` were built on the combination this ADR describes; fixed by PR #854).
  **Blocked-on / related:** #855 (world route drops content the instance route draws).
  **Waiting on Option 2:** `qa_stream_multi_asset.tscn`.
- **Verified against:** `origin/master` @ **`a04472a82cf`**. Every file:line in this
  document was read at that commit. §2 records which of the issue's original citations had
  drifted. The round-5 re-derivation of §6.4 was read at `origin/master` @ **`c73570c840f`**;
  every file it cites (`scene/3d/node_3d.cpp`, `scene/main/viewport.cpp`, `scene/main/node.cpp`,
  `core/object/object.h`, `scene/main/scene_tree.cpp`, both node `.cpp`s and
  `core/gaussian_splat_scene_director.cpp`) is byte-identical between the two commits, so the
  line numbers below hold at both. **Round 6 re-checked that blob identity** (`git rev-parse
  a04472a82cf:<path>` vs `c73570c840f:<path>` for all seven files — identical) and re-read the
  registration paths. It found one **factual error** in §6.1's account of conjunct S:
  `last_known_scenario` is written from a **second** site on the world node,
  `_ensure_renderer()` (`nodes/gaussian_splat_world_3d.cpp:326-328`), which the previous
  revisions did not enumerate. §6.1 now records all the write sites, and §6.4 was re-derived
  in full against the corrected reading — which moved **T8d** off the deliberately-unkilled
  list and turned "drop conjunct P entirely" from an unkilled row into a killed one.
  **Round 8 re-anchored the whole document.** `origin/master` has since moved to
  **`924bef76b9b`**; every one of the seventeen files this ADR cites (both node `.cpp`/`.h`s,
  `core/gaussian_splat_scene_director.cpp`/`.h`, `core/gaussian_splat_world.cpp`,
  `core/gaussian_splat_asset.cpp`, `core/gs_project_settings.h`,
  `core/gaussian_splat_manager.cpp`, `renderer/gaussian_splat_renderer.cpp`,
  `renderer/resident_instance_contract_publisher.cpp`, `scene/3d/node_3d.cpp`,
  `scene/main/viewport.cpp`, `scene/main/node.cpp`, `scene/main/scene_tree.cpp`,
  `core/object/object.h`) was blob-compared `a04472a82cf:<path>` vs `924bef76b9b:<path>` and is
  **byte-identical**, so every line number below holds at current master. Round 8 also found a
  **third** product bug in the class this spec describes, **#869**: `last_known_scenario` (S) is
  written *before* `submit_world_submission()` can reject, so a previously-applied world node
  that loses arbitration on a re-apply ends up with S naming a scenario it holds no submission
  in **while I stays `true`**. §6.1 records the bounded false negative that produces;
  **#869 is not a prerequisite** and §9 justifies why, against the same test that made #862 one.

## 1. What a user sees

Place a `GaussianSplatWorld3D` and a `GaussianSplatNode3D` in the same scene. One of the
two renders nothing. In the configuration reported on #788 the *world* renders nothing,
there is no error, and the only signal is a single `WARN_PRINT_ONCE`:

```text
[GaussianSplatRenderer] Resident route rejected
(reason=submission_hint_resident:instance_submission:resident_no_instances);
frame skipped to preserve single-route-per-frame contract.
```

Hiding either node does not hand the frame back to the other. The two node types are never
independently observable in one scene.

For a renderer aiming at a public alpha this is the wrong failure shape: the behaviour is
contract-consistent, but "I added a second node and my scene went black, with one line in a
log most users never open" is not a diagnosable outcome.

## 2. Citation audit — what had drifted

The issue's mechanism paragraph was written against an earlier tree. Re-checked at
`a04472a82cf`:

| Claim in #788 | Cited as | Actual at `a04472a82cf` | Verdict |
| --- | --- | --- | --- |
| "world submissions only" scope doc | `core/gs_project_settings.h:234-252` | doc block `:235-246`, function `get_streaming_route_policy()` at `:247` | **drifted** (off by one at the start, overshoots the end); claim itself accurate |
| world derives its hint from `route_policy` | `nodes/gaussian_splat_world_3d.cpp:505-517` | `:510-514` (`has_desired_residency_hint = true` at `:510`, policy read at `:511`, hint ternary `:512-514`) | **drifted**; claim accurate |
| `resident_no_instances` | `renderer/resident_instance_contract_publisher.cpp:374` | `:400` (guard `:398-404`) | **drifted by 26 lines**; claim accurate |
| the frame skip | `renderer/gaussian_splat_renderer.cpp:2478` | reason string built at `:2474`, `WARN_PRINT_ONCE` at `:2508`, `return` at `:2513` | **drifted by 30 lines**; claim accurate |
| repo already calls the combination unproven, in two places | `qa_test_runner.gd`, `tests/runtime/test_mixed_residency_routing.gd:3-5` | `qa_test_runner.gd:53-55` and `test_mixed_residency_routing.gd:3-5` | **both confirmed verbatim** |

One claim is **not** merely drifted but **imprecise**, and the correction changes what a fix
has to handle — see §3.2.

### 2.1 PR #854 (issue #785) is open, not merged

At `a04472a82cf`, PR #854 has **not** landed. It rewrites `qa_test_runner.gd` and replaces
`qa_visual_diff.tscn` / `qa_sh_rotation.tscn` with four route-separated scenes. Two
consequences for this ADR:

- The `qa_test_runner.gd` line numbers above will move when #854 merges. The
  **`qa_stream_multi_asset.tscn` entry that #788 cites is not one of the entries #854
  removes** — #854 deletes only the two `#785` entries. The "disabled until the runtime
  surface can prove true resident/streaming coexistence" reason survives #854 and remains
  the repo's standing statement that this combination is unproven.
- #854 does **not** make the combination work. It removes the two QA scenes that depended on
  it. Nothing in the repo after #854 exercises world+instance coexistence.

## 3. Mechanism, as the code actually reads

### 3.1 The route is chosen once per frame, per renderer

`GaussianSplatRenderer::should_prefer_resident_backend()`
(`renderer/gaussian_splat_renderer.cpp:2842-2875`) resolves one backend for the whole frame:

1. `route_policy == GS_ROUTE_RESIDENT` → resident, reason `requested_resident_policy`
   (`:2849`).
2. otherwise ask the scene director for a single submission residency hint; no hint →
   streaming.
3. hint `RESIDENT` → resident, reason `submission_hint_resident:<source>` (`:2864`).

`build_frame_backend_plan()` (`:1600-1657`) turns that into the frame's plan, and
`render_scene_instance()` commits: if the resident route is chosen and the resident contract
cannot be published, the frame is **skipped**, deliberately, rather than falling through to
the other backend — `single_route_per_frame` and `alternate_backend_fallback_forbidden` are
read at `:2505-2506` and drive the skip at `:2508-2513`. Resident publication fails with
`resident_no_instances` when the instance list is empty
(`renderer/resident_instance_contract_publisher.cpp:398-404`).

The project default is `route_policy = 1` = `GS_ROUTE_STREAMING`
(`core/gaussian_splat_manager.cpp:1008`; enum at `core/gs_project_settings.h:196-197`).

### 3.2 Correction: the world hint has *precedence*, and the failure means it was absent

#788 says the instance node "always publishes RESIDENT" and that therefore "with both
present the renderer commits to RESIDENT". The precedence in
`GaussianSplatSceneDirector::get_submission_residency_hint_for_renderer()`
(`core/gaussian_splat_scene_director.cpp:2811-2859`) is the other way round:

- If a world submission is **active** for this renderer *and*
  `record_has_renderable_payload()` is true *and* it carries a hint, the **world's** hint
  wins and the source is `"world_submission"` (`:2817-2827`).
- Only if that test fails does it fall back to scanning the instance records
  (`:2830-2852`), whose hint is `RESIDENT` (source `"instance_submission"`, `:2849`) unless
  the instance records disagree with each other (`"mixed_instance_submissions"`, `:2841`,
  which returns *no* hint).

The reason string recorded on #788 is `submission_hint_resident:instance_submission`. That
string is only reachable through the **fallback** branch, and only when
`route_policy != RESIDENT`. So the measured failure proves something sharper than the issue
states: **at the moment the route was decided the world submission failed the
`is_active() && record_has_renderable_payload() && has_desired_residency_hint` gate
(`:2819-2820`), even though the world node was present in the scene.** The world node sets
`has_desired_residency_hint = true` unconditionally on every submission it publishes
(`nodes/gaussian_splat_world_3d.cpp:510`), so the gate failed on `is_active()` or on
`record_has_renderable_payload()` — that is, on **submission state**.

**The cause of that state is not established, and this ADR does not name one.** Several
steady, non-transient states produce the same reason string:

- **the submission was never committed.** `submit_world_submission()` rejects when a live
  world already owns that scenario (`core/gaussian_splat_scene_director.cpp:2390-2399`), and
  the node then returns without marking itself active
  (`nodes/gaussian_splat_world_3d.cpp:516-522`). Persistent for as long as both worlds exist.
- **the record was reset** by release, cross-scenario eviction or teardown
  (`SubmissionStore::reset()`, `core/gaussian_splat_scene_director.h:752`).
- **the payload is not renderable at all.** `record_has_renderable_payload()`
  (`:843-851`) requires `gaussian_data` with `get_count() > 0` **or** a valid
  `payload_source` with `get_count() > 0`; a world satisfying neither fails the gate for the
  whole session, not for a startup window.

A load-timing race is one further candidate, not the measured cause. Nothing in this repo
distinguishes them; §8 records what capture would. The user-facing text in §6.2 therefore
names **no** cause.

What *is* established is the precedence and its two consequences, and they are asymmetric:

- when the world submission passes the gate, the **world's** hint wins deterministically
  (`:2819-2825`) and the **instance** node's content is silently dropped — which is exactly
  the symmetry #788 reports ("forcing `route_policy=1` makes the instance node's capture
  render the *world's* splats");
- when it fails the gate, the instance hint decides (`:2846-2851`) and the **world** is the
  one skipped (`resident_no_instances` when the instance content is also unavailable, e.g.
  hidden or empty) — the configuration reported on #788.

**Which node type disappears therefore depends on the world submission's state at the moment
the route is decided — a state the user cannot see and this ADR cannot attribute.** That is
worse than a fixed precedence and is a first-class reason not to leave the combination
silent.

### 3.3 Why hiding a node does not help

Two independent reasons, both structural:

- The instance-hint fallback loop at `core/gaussian_splat_scene_director.cpp:2830-2846`
  iterates `world->instance_store.records()` and filters only on
  `record.has_desired_residency_hint`. It **does not check `record.visible`**, although the
  same store's `visible` flag is honoured elsewhere in the same file (`:1603`, `:1754`,
  `:1929`, `:2075`, `:2923`). A hidden `GaussianSplatNode3D` still steers the global route.
- Even if it did, the route decision is per-frame and global. `visible` decides whether the
  shared composite draws, not which submission owns the frame.

The first point is a defect in its own right and is recorded as follow-up work in §9. It is
**not** fixed by this ADR, and the Option 1 warning is deliberately specified to be
visibility-independent so that it does not quietly depend on it.

### 3.4 `route_policy` does not resolve the conflict either

Setting `route_policy = 0` (`GS_ROUTE_RESIDENT`) makes `should_prefer_resident_backend()`
return true at step 1 (`:2847-2851`). The resident route then renders whatever instances
the director builds — the `GaussianSplatNode3D` content — and the world's streaming payload
is not drawn. Setting it to `1` does not restore symmetry: if the world submission passes the
hint gate the world takes the frame and the instance content is dropped; if it does not, the
instance hint takes the frame and the world is dropped (§3.2).
**No value of `route_policy` renders both.** The conflict is `route_policy`-independent,
which is why the warning specified in §6 does not read `route_policy` at all.

## 4. Why this is a decision and not just a bug

The renderer is doing what its contract says. `core/gs_project_settings.h:235-246` states
the scope explicitly and even anticipates the question ("If per-node backend steering is
ever needed, introduce a narrow per-world-group setting rather than broadening this one").
The repo has said in two places that coexistence is unproven
(`tests/examples/godot/test_project/scripts/qa_test_runner.gd:53-55`,
`tests/runtime/test_mixed_residency_routing.gd:3-5`). What is missing is not correctness —
it is that **nothing stops a user from authoring the scene, and nothing tells them.**

## 5. Options

### 5.1 Option 1 — make it loud (**accepted, interim**)

Surface the conflict as an editor configuration warning on both node types, at author time,
in the scene tree, where a user is already looking. Cheap (R1), reversible, and orthogonal
to Options 2 and 3. It changes no render behaviour and breaks no existing scene: a warning
triangle is additive.

Accepted. Specified in §6.

**Accepted as the decision — but not implementable until #862 lands.** The decision (warn now,
route later) is unchanged by rounds 5 and 6; what those rounds established is that the
*deliverable* has two product-bug dependencies, and they are not symmetric:

- **#862 is a hard prerequisite.** The predicate has to answer "does this node hold a
  submission that steers the frame's route". `SubmissionStore::store_submission()` copies the
  payload `Ref`s into the record (`core/gaussian_splat_scene_director.cpp:828-841`) and
  `GaussianSplatWorld3D` never connects to the resource's `changed` signal (verified: the
  translation unit contains **no** `connect` call at all) — its only resubmission path,
  `_resubmit_world_submission_if_registered()` (`nodes/gaussian_splat_world_3d.cpp:399-415`),
  is reached solely from the seven renderer-parity setters at `:266-299`, none of which touch
  the payload. So the resource and the record drift apart in both directions:
  - **adding** a payload → P true, record still empty → the warning fires while the world
    submission is *not* steering (false positive, §6.1);
  - **removing** a payload (`set_gaussian_data(null)` or a zero-count `Ref` on an applied
    world whose `gaussian_data` was its only payload) → P false, record still holds the old
    non-empty `Ref`, so `record_has_renderable_payload()`
    (`core/gaussian_splat_scene_director.cpp:843-851`) is still true, the world hint still
    wins the gate at `:2818-2827`, and the world submission **still steers the route** while
    the warning goes silent (false negative, §6.4 T13).

  The false positive was recorded as a bounded, one-transition caveat. The false negative is
  not bounded in the same way: it is the diagnostic disappearing precisely when the failure it
  diagnoses is live. **Ship the warning on top of #862, not before it.**
- **#863 is not a hard prerequisite, and this ADR does not claim it is.** Its effect is on the
  *remedy*, not the predicate: toggling `own_world_3d` on the viewport containing the world
  node separates the two `World3D`s but leaves the world node's submission registered in the
  old scenario, so the conflict — and the warning — survive the exact action §6.2 recommends
  (this is §6.4's T10b). Round 6 verified that **re-applying the world node afterwards does
  clear it**: `_register_shared_renderer()` resolves the new scenario at
  `nodes/gaussian_splat_world_3d.cpp:488` and `submit_world_submission()`'s phase 3
  re-resolves the owner's previous world and **resets** it
  (`core/gaussian_splat_scene_director.cpp:2509-2517`), so the stranded submission is
  released, not duplicated. §6.2's text therefore names that step. #863 turns a one-step
  workaround into a two-step one; it does not make the warning wrong.

- **#869 is not a hard prerequisite either, and the reasoning is neither #862's nor #863's.**
  Found in round 8: `last_known_scenario` (conjunct S) is written before
  `submit_world_submission()` can reject, so a previously-applied world node re-applied into a
  scenario another live world owns keeps **I = `true`** while **S** names a scenario it holds no
  submission in. That is a false negative of the #862 kind — the warning goes quiet while the
  stale submission still steers — but it is **not reachable in the scene this warning targets**:
  the rejection requires a *second* live world node owning the destination scenario, so the
  state needs two world nodes, a `World3D` migration and a re-apply. §9 works the comparison
  through against the same test that made #862 a blocker, and records the two further checks
  (#869 does not break the §6.2 remedy, and it does not change §6.1 when it lands).

None of the three fixes is in scope for the Option 1 PR — all three change what the renderer
draws or when it draws it, and this one changes only what the editor says (§9).

### 5.2 Option 2 — make it work (**accepted as target, deferred**)

Genuine per-submission routing. This is the real feature and the one
`qa_stream_multi_asset.tscn` is explicitly waiting on. Deferred, not declined, for three
reasons:

1. **The choke point is a single-value API.** `get_submission_residency_hint_for_renderer()`
   returns *one* `int32_t` for a renderer (`:2811`), `FrameBackendPlan` carries *one*
   backend (`renderer/gaussian_splat_renderer.cpp:1600-1657`), and `RenderRouteDecision`
   carries `single_route_per_frame` / `alternate_backend_fallback_forbidden` as invariants
   that the skip path *reads* (`:2505-2506`). Per-submission routing is not a hint change;
   it is a change to what a frame *is*.
2. **Compositing is the hard part, not routing.** The module composites once at end of
   frame with one colour and one depth per pixel. Two routes that cull, sort and raster
   independently and composite independently cannot produce correct inter-route occlusion
   or blending for transparent splats — they would need a merged sort or an explicitly
   documented ordering restriction. That is a design question with its own ADR, not a
   follow-up commit.
3. **Ordering constraint: #855 first.** #855 measured the world route dropping content the
   instance route draws (SSIM 0.8813 at one orbit angle, reproduced bit-identically at two
   settle times). Building a coexistence A/B on top of two routes that already disagree
   would produce a gate that cannot distinguish a routing defect from the known divergence.
   **#855 must be root-caused before Option 2 is scheduled.**

### 5.3 Option 3 — make it impossible (**rejected**)

Hard-reject the combination with an error naming both nodes.

Evaluated honestly, and rejected:

- **It breaks scenes that exist, on upgrade.** Today the combination is not uniformly
  black: per §3.2, one of the two node types *does* render, and which one depends on the
  world submission's state. A user whose scene currently shows their instance content
  would, on upgrading, get a hard error and nothing at all. A diagnostic that turns partial
  output into no output is a regression, and the module has no scene-migration mechanism to
  soften it.
- **There is nothing to reject at the point it would matter.** The nodes are already in the
  user's saved `.tscn`. A runtime `ERR_FAIL` cannot un-author the scene; it can only make
  the same failure louder and less recoverable than the warning Option 1 already gives.
- **It forecloses Option 2.** Any implementation of per-submission routing must stand up
  exactly this combination, in tests and in `qa_stream_multi_asset.tscn`. A rejection in the
  node layer becomes the first thing Option 2 has to delete, and every intermediate slice
  has to work around it.
- **It is not the same as the module's existing fail-closed precedent.** The module does
  hard-reject an unsupported configuration elsewhere (rejecting no-manager sorters at init,
  #764) — but that is an internal, opt-in, non-default code path, not a user-authored scene
  composition. `AGENTS.md`'s "do not weaken a guard" rule protects measured invariants; it
  does not argue for converting a product limitation into a crash.

**The honest counterpoint, recorded:** a hard error would guarantee nobody ships a silently
black scene, and Option 1 does not — a user can ignore a warning triangle. This is accepted
as a residual risk, mitigated by the warning naming the observable consequence explicitly
(§6.2) and by the documented workaround. If evidence later shows users shipping the broken
combination anyway, Option 3 can be revisited as an *export-time* check rather than a
runtime error, which would break no editing session.

## 6. Implementation specification — Option 1

Implementable as written. No part of this section was executed or built; §8 lists what
remains unverified.

### 6.1 Classes and detection

**Both** node classes get the warning, symmetrically. A user who clicks either node must see
it; a warning on only one is a coin-flip on which node they inspect first.

- `GaussianSplatNode3D` already overrides `get_configuration_warnings()`
  (`modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:1810-1851`, bound at
  `:234`). Append the new warning there.
- `GaussianSplatWorld3D` has **no** override today (confirmed: no
  `get_configuration_warnings` in `nodes/gaussian_splat_world_3d.h`). Add
  `PackedStringArray get_configuration_warnings() const override;`, chain to
  `Node3D::get_configuration_warnings()`, and mirror the `ClassDB::bind_method` that
  `GaussianSplatNode3D` uses at `:234` so GDScript-side tests can read it.
- `GaussianSplatDynamicInstance3D` needs **no change**: it derives from
  `GaussianSplatNode3D` and its override already chains
  (`nodes/gaussian_splat_dynamic_instance_3d.cpp:8`).

**Discovery mechanism — SceneTree groups, scoped by `World3D`.** Do not scan the scene tree.

- Two non-persistent group names, declared once as shared constants (e.g. in
  `nodes/gaussian_splat_node_3d.h` or a small shared header):
  `"__gs_world_submission_nodes"` and `"__gs_instance_submission_nodes"`.
- Each class calls `add_to_group(<its own group>, /*persistent=*/false)` **once, in its
  constructor**. Godot registers grouped nodes with the `SceneTree` on tree entry
  (`scene/main/node.cpp:337-339`) and removes them on tree exit (`:424-427`), so
  `SceneTree::get_nodes_in_group()` returns exactly the in-tree nodes with no bookkeeping of
  our own. `persistent = false` keeps the group out of the saved `.tscn`. **The two are not
  symmetric in time** — registration happens *before* `NOTIFICATION_ENTER_TREE`, removal
  *after* `NOTIFICATION_EXIT_TREE` — and §6.3 turns on that asymmetry.
- The condition, evaluated only when `get_configuration_warnings()` is called. It is stated
  **once, positively**, as a single predicate over one node, and the warning is that predicate
  holding for two nodes of different types in one scenario. Earlier drafts of this ADR
  accumulated exclusions instead ("assigned" → "renderable payload" → "…and not inactive"),
  and each round found another state the exclusion list had missed. The positive form below is
  what the exclusions were approximating:

  ```text
  WOULD_STEER(N)  ≡   N is in a state in which the director would hold,
                      for N, a submission that steers the frame's route

                  ≡   N.is_inside_tree()                                    (R1)
                  AND N.is_inside_world()                                   (R2)
                  AND registered_scenario(N).is_valid()                     (S)
                  AND payload(N)                                            (P)
                  AND submission_intent_live(N)                             (I)

  warn on N       ≡   WOULD_STEER(N)
                  AND ∃ M in the *other* group with
                          WOULD_STEER(M)
                      AND registered_scenario(M) == registered_scenario(N)
  ```

  **§6.4's mutation table is a function of this predicate.** Every "Killed by" entry there is
  the claim "*this conjunct is the one that changes value in this state*", so changing,
  adding or removing a conjunct here silently invalidates rows over there. Two rounds have
  already shipped a row that the predicate had outgrown. **Any edit to this block requires
  re-deriving §6.4 in full** — see the rules under "The table is coupled to the predicate".

  Every conjunct is a mirror of a guard on the path that actually produces the submission, and
  is listed here with that guard. Nothing is in the predicate that is not:

  | Conjunct | Mirrors, on the world node | Mirrors, on the instance node |
  | --- | --- | --- |
  | R1 | `_register_shared_renderer()` returns early when `!is_inside_tree()` (`nodes/gaussian_splat_world_3d.cpp:475-477`) | `_register_instance_in_director()` returns early when `!in_tree` (`nodes/gaussian_splat_node_3d.cpp:2590-2592`) |
  | R2 | (no direct guard — `_register_shared_renderer()` gates only on the tree; see S) | `_register_instance_in_director()` returns early when `!in_world` (`nodes/gaussian_splat_node_3d.cpp:2590-2592`) |
  | S | `last_known_scenario`, written at `_register_shared_renderer()` (`nodes/gaussian_splat_world_3d.cpp:488-493`) **and** at `_ensure_renderer()` (`:326-328`) — see the write-site list below | `last_known_scenario`, written only inside `_register_instance_in_director()` (`nodes/gaussian_splat_node_3d.cpp:2622-2625`), after its asset-null early-return at `:2607-2609` |
  | P | `SubmissionStore::record_has_renderable_payload()` (below) | the asset resolution in `_register_instance_in_director()` (below) |
  | I | `was_world_submission_active` (below) | **constant true** — the class has no apply gate (below) |

  **`registered_scenario(N)` (S) is the scenario the node last registered into, not the one it
  resolves now — and that distinction is the round-4 correction.** It is the node's existing
  `last_known_scenario` cache (`nodes/gaussian_splat_world_3d.h:27`,
  `nodes/gaussian_splat_node_3d.h:215`), exposed through a public const accessor (e.g.
  `RID get_registered_scenario() const`) on both classes, the same way conjunct I exposes
  `was_world_submission_active`. The condition therefore never calls `get_world_3d()` at all.

  The reason is that the two can *disagree*, and when they do the resolved one is wrong.
  `GaussianSplatNode3D` migrates its registration on a world switch — `NOTIFICATION_EXIT_WORLD`
  unregisters and `NOTIFICATION_ENTER_WORLD` re-registers
  (`nodes/gaussian_splat_node_3d.cpp:445-464`), refreshing `last_known_scenario` at `:2622-2625`
  — but `GaussianSplatWorld3D` **handles neither notification** (its `_notification` at `:73-215`
  has no `ENTER_WORLD`/`EXIT_WORLD` case) and its submission is released only from
  `NOTIFICATION_EXIT_TREE` (`:126`) and `clear_world()` (`:307`). So when a viewport containing
  a world node takes its own world (`Viewport::set_use_own_world_3d()`,
  `scene/main/viewport.cpp:4740`; `set_world_3d()`, `:4683` — neither touches the tree), the
  node resolves the **new** scenario while its submission is still registered in, and still
  arbitrating over, the **old** one. A resolved-scenario comparison would drop the warning on
  both nodes while the conflict is still live in the old world: a false negative. That defect is
  filed as **#863**; S makes the predicate correct **without waiting for it**, and stays correct
  after it lands, because a migrating world node updates `last_known_scenario` at the same
  moment it moves the submission.

  **Every write site of S, enumerated — the round-6 correction.** Rounds 4 and 5 asserted that
  `last_known_scenario` is "written only inside `_register_shared_renderer()`". That is
  **false on the world node**, and the omission invalidated two rows of §6.4. The complete
  list, read at `c73570c840f`:

  | Class | Site | Value written | Reached from |
  | --- | --- | --- | --- |
  | `GaussianSplatWorld3D` | `_register_shared_renderer()` `:491-493` | the scenario being **submitted to** (`:488`) — written *before* `submit_world_submission()` at `:516` and **not** rolled back when it rejects at `:521` (#869, round 8) | `_apply_world_internal()` `:452`, `_resubmit_world_submission_if_registered()` `:414` |
  | `GaussianSplatWorld3D` | `_ensure_renderer()` `:326-328` | the **resolved** scenario (`get_world_3d()`) | `NOTIFICATION_READY` `:104`, `apply_world()` `:302`, the tail of `_register_shared_renderer()` `:525`, and `_notification_process()` `:223` |
  | `GaussianSplatNode3D` | `_register_instance_in_director()` `:2622-2625` | the **resolved** scenario | `_notification_enter_world()` `:383` → `_register_shared_renderer()` `:2903`, and only past that function's own no-data early-return at `:2897-2901` and this one's asset-null early-return at `:2607-2609` |

  Both classes clear it only at `NOTIFICATION_PREDELETE`
  (`nodes/gaussian_splat_world_3d.cpp:186`, `nodes/gaussian_splat_node_3d.cpp:519`).

  **S is never stale in the direction that produces a false positive *except* in the one state
  round 8 found (#869), which is enumerated as the last item below** — that "never" was stated
  unqualified for four rounds and was wrong. The derivation is now the following, and three of
  its steps are new:
  - It survives tree exit, which releases the submission but leaves the cache — and R1
    excludes that state.
  - It survives `clear_world()`, which releases the submission but leaves the cache — and I
    excludes that state.
  - It survives a world switch on the world node — and that is the case where it is *right*,
    because the submission did not move either.
  - **New:** `_ensure_renderer()` at `NOTIFICATION_READY` (`:104`) writes S **before** the
    `auto_apply_on_ready` test at `:108` and therefore before any submission exists. So a
    never-applied world node has a *valid* S. That is not a false positive, because I is false
    in that state — but it is exactly the fact that makes **T8d killable** (§6.4), which rounds
    4 and 5 got backwards.
  - **New, and the one genuine gap:** `_notification_process()` calls `_ensure_renderer()` at
    `:223`, which moves S to the **resolved** scenario without moving the submission. It is
    reachable only when `set_process(true)` ran at `:118-120`, i.e. only under
    `OS::has_feature("headless")`, and the handler re-checks the same feature at `:219-220`.
    The editor is not headless, so this cannot affect the warning where a user reads it. It
    **can** affect a headless `[SceneTree]` test that iterates the tree: after a #863
    stranding, one `tree->process()` moves S to the new scenario while the submission stays in
    the old one, which turns the conflict invisible. That is a false *negative*, never a false
    positive — and it is why §6.4's T10b carries an explicit "do not call `tree->process()`"
    setup constraint.
  - **New in round 8, and the one state where S *is* stale in the false-positive direction —
    filed as #869.** S is written at `:491-493` **before** `submit_world_submission()` runs at
    `:516`, and the arbitration rejection returns at `:521` without rolling it back. (On the
    `apply_world()` path S has in fact already moved one step earlier still: `apply_world()`
    `:301-303` calls `_ensure_renderer()` *first*, which writes S from the resolved world at
    `:326-328` before `_apply_world_internal()` is even entered.) So take a world node that
    applied successfully in scenario A, then comes to resolve scenario B — a viewport world
    switch (#863) — and re-apply it. If B's submission slot is already held by **another live
    world node**, the director rejects at
    `core/gaussian_splat_scene_director.cpp:2392-2401`, and phase 3's
    `previous_world->submission_store.reset()` (`:2509-2517`) is on the **commit** path only, so
    A's submission is left installed. The node ends with **S = B, I still `true`** (I is set at
    `:524`, past the rejection's return, and cleared only by `clear_world()` at `:312`), and its
    submission still steering **A**. Both directions follow: **false negative in A** — an
    instance node registered in A is genuinely being dropped by this node's submission, the
    scenario equality fails, and neither node warns; and a **misattributed fire in B** — the
    node warns against a peer in B whose content is in fact being dropped by the *incumbent*
    world node, not by this one. The predicate cannot close this from node-side state: after the
    rejection nothing on the node distinguishes "registered in the scenario I last resolved"
    from "registered where I was last *accepted*", and the only alternative input is the live
    director query §6.1 rejects two bullets down. **It is therefore a product bug, fixed in
    #869, not a predicate change here.** It is bounded, and the bound is what keeps it off the
    prerequisite list (§9): the rejection at `:2392-2401` fires *only* when a second, live
    `GaussianSplatWorld3D` already owns the destination scenario, so the state needs **two**
    world nodes plus the instance node plus a world migration plus a re-apply — and in that
    scene the user's undiagnosed problem is already two world nodes in one scenario, which §6.1
    states below that this ADR does not diagnose at all.

  `is_inside_world()` (R2) **stays**, and now for a different reason than the previous revision
  gave. It is no longer a guard against `get_world_3d()` printing an engine error, because the
  condition no longer calls it. It is the one gap S cannot close on the instance side: that
  node's `EXIT_WORLD` unregisters at `:449-464` **without** clearing `last_known_scenario`, so
  between `EXIT_WORLD` and `ENTER_WORLD` it is in the tree, holds no record, and still reports a
  valid stale S. Every refresh §6.3 specifies is deferred past that window, but
  `get_configuration_warnings()` is pull-based and the editor may call it at any time, so the
  conjunct is kept. It also mirrors a real registration guard on that class (`!in_world`,
  `:2590-2592`).

  Scoping by scenario RID — not by edited-scene root — is what makes this correct: the
  director keys its `world_registry` by scenario
  (`core/gaussian_splat_scene_director.cpp:2865-2870`), so two nodes in different `World3D`s
  (a `SubViewport` with `own_world_3d`) genuinely do not conflict and must not warn.

- **`payload(N)` (P)** means, deliberately, *the assigned resource carries content the director
  can actually register* — not *is visible*, and **not merely *has a resource assigned***:
  - `GaussianSplatWorld3D`: `get_world().is_valid()` **and** the world's payload is
    non-empty, mirroring `SubmissionStore::record_has_renderable_payload()`
    (`core/gaussian_splat_scene_director.cpp:843-851`) against the exact two values
    `_register_shared_renderer()` publishes (`nodes/gaussian_splat_world_3d.cpp:494-495`):
    a non-null `world->get_gaussian_data()` with `get_count() > 0`, **or** a non-null
    `world->get_chunk_payload_source()` whose own `is_valid()` is true and whose
    `get_count() > 0`.

    **P mirrors the record only while the record tracks the resource, and today it does not —
    dependency on #862.** `SubmissionStore::store_submission()` copies the payload `Ref`s into
    the `WorldSubmissionRecord` (`core/gaussian_splat_scene_director.cpp:828-841`) and nothing
    re-publishes when the *assigned* `GaussianSplatWorld` is mutated afterwards:
    `_resubmit_world_submission_if_registered()` (`nodes/gaussian_splat_world_3d.cpp:399-415`)
    is called only from the seven renderer-parity setters at `:266-299`, and the class never
    connects to the resource's `changed` signal. So after
    `world_res->set_gaussian_data(<non-empty>)` on an already-applied world node, P reads the
    new resource and returns true while `record_has_renderable_payload()` reads the old empty
    `Ref` and returns false — the world hint loses the gate at `:2818-2827` and the submission
    **does not steer the route**. In that state `WOULD_STEER` as *defined* at the top of this
    section is false while P as *implemented* is true, and the §6.2 warning is a false
    positive: its diagnosis ("only one of the two is drawn") is still observably right, but its
    prescribed remedy — separate the two `World3D`s — would not restore the world content,
    because the content is absent for a reason that has nothing to do with the other node.
    **The same drift runs the other way, and that direction is why #862 is a prerequisite
    rather than a caveat (round 6).** Call `world_res->set_gaussian_data(Ref<GaussianData>())`
    — or install a zero-count `Ref` — on an already-applied world node whose `gaussian_data`
    was its only payload. `GaussianSplatWorld::set_gaussian_data()`
    (`core/gaussian_splat_world.cpp:107`) assigns the null `Ref` at `:108`, skips its metadata
    block (`:109-118`, guarded on `is_valid()`) and emits `changed` at `:120`. Nothing
    resubmits — the §6.3 `changed` handler is specified as **refresh-only**, deliberately, so
    that a diagnostic PR does not change what the renderer draws. The result is the mirror
    image of the case above and it is strictly worse:

    - P reads the **new** resource → false → `WOULD_STEER` false → **the warning disappears
      on both nodes**;
    - the record still holds the **old** non-empty `gaussian_data` `Ref`
      (`SubmissionStore::store_submission()` copied it at
      `core/gaussian_splat_scene_director.cpp:830`), so
      `record_has_renderable_payload()` (`:843-851`) is still true and `active` is still true
      — the world hint still wins the gate at `:2818-2827` and **the stale submission is still
      steering the frame's route**, with the instance node's content still being dropped.

    A false positive tells the user something true about their scene with an unhelpful remedy.
    **This is a false negative: the diagnostic goes silent at the moment the failure it exists
    to report is live.** No wording change reaches it, and the only two fixes are #862 or
    basing P on the submitted payload — which this ADR rejects two bullets down (a live
    director query makes the warning blink). **#862 is therefore recorded in the decision
    section as a prerequisite of implementing Option 1.**

    **This ADR does not fix #862**, and must not: it is a product bug in
    `GaussianSplatWorld3D`, filed with its own two-directional mutation proof. The predicate
    is exact once it lands. §6.4's **T11** is the case that sits in the false-positive
    divergence and **T13** the one that sits in the false-negative divergence; both are
    specified as proofs of the `changed` **wiring** only, and the notes on those rows say so.
  - `GaussianSplatNode3D`: **mirror the asset resolution in
    `_register_instance_in_director()`** (`nodes/gaussian_splat_node_3d.cpp:2593-2606`) and
    require a non-zero count on whichever member that resolution picks:
    1. `splat_asset.is_valid()` → `splat_asset->get_splat_count() > 0`;
    2. else `renderer_data.is_valid()` → `renderer_data->get_count() > 0` (registration
       rebuilds `runtime_asset` from it);
    3. else `runtime_asset.is_valid()` → `runtime_asset->get_splat_count() > 0`;
    4. else: no payload.

    An OR over the three members is **wrong**, not just imprecise: step 1 wins even when
    `splat_asset` is empty and `renderer_data` is full, and an empty `splat_asset` is
    precisely what registration then fails on. The predicate must reproduce the precedence,
    not the union.

- **`submission_intent_live(N)` (I)** means *the node has applied its content and has not
  deliberately stopped showing it*. A payload-only predicate is not enough, because
  `GaussianSplatWorld3D` has two states in which it holds a fully renderable `GaussianSplatWorld`
  and still submits nothing:

  - **Never applied.** `NOTIFICATION_READY` (`nodes/gaussian_splat_world_3d.cpp:100`) calls
    `apply_world()` only `if (auto_apply_on_ready)` (`:108-113`). With the property set to
    `false` — the serialized default is `true` (`nodes/gaussian_splat_world_3d.h:29`), so this
    is an explicit author choice — the first `READY` deliberately does not apply.
    `NOTIFICATION_ENTER_TREE` (`:79`) does not rescue it either: its re-apply is gated on
    `was_world_submission_active` (`:96-98`), which is only ever set at `:524`, after
    `submit_world_submission()` has succeeded.
  - **Explicitly cleared.** `clear_world()` (`:306-317`) unregisters the submission and sets
    `was_world_submission_active = false` (`:312`) **but leaves `world` assigned** — the `Ref`
    is never nulled — precisely so the clear is not auto-resumed on the next `ENTER_TREE`
    (`:308-311`). P therefore still holds for a node the user has switched off.

  In both states the *other* node type renders normally, so a warning would tell the user that
  half their scene is being dropped while it is in fact on screen.

  **Both states are the same field.** `was_world_submission_active` (`nodes/gaussian_splat_world_3d.h:44`)
  is set true only at `:524` and false only at `:312`, so it *is* "applied and not cleared".
  I is therefore **one** conjunct, not two exclusions — which is the point of stating the
  predicate positively. The field is private; the peer walk reads it on the *other* node, so
  the implementation adds a public const accessor (e.g.
  `bool has_live_submission_intent() const { return was_world_submission_active; }`).

  **On `GaussianSplatNode3D`, I is constant `true`, and that asymmetry is real, not an
  oversight.** `_register_instance_in_director()` (`nodes/gaussian_splat_node_3d.cpp:2577`)
  has no apply gate: no `auto_*` flag, and no clear path that leaves the asset assigned —
  clearing the asset clears `splat_asset`, which P already catches. Do not add a symmetric
  exclusion on the instance side; there is no code behind it, and a conjunct with no guard to
  mirror is the thing this predicate is built to avoid.

- **Do not gate on a *live* director or renderer query** (`_is_renderer_shared_with_other_content()`,
  `nodes/gaussian_splat_node_3d.cpp:42-59`, or `get_world_submission()` at
  `core/gaussian_splat_scene_director.cpp:2775`). Those are the right question at runtime and
  the wrong one at author time: they change without the node changing — when a *peer*
  registers, when a resource finishes loading, when a renderer is torn down — so the warning
  would blink. They may be used as an *additional* runtime log, never as the condition.

  **I is the one director-derived input the condition is allowed, and the narrowing is
  deliberate.** `was_world_submission_active` is node-side and *latched*: it moves only when
  this node applies or clears. It is not a live query and does not track peer churn. It does
  inherit one dependency from the director — `submit_world_submission()` returns false when
  another world already owns the scenario (`nodes/gaussian_splat_world_3d.cpp:516-522`).

  **What that dependency does to I is not uniform, and the previous revision stated only the
  half that is harmless (round-8 correction).** The claim it made — "a world node that lost
  world-vs-world arbitration reads I = false and stays silent" — is true **only of a node that
  has never successfully applied**. `was_world_submission_active` is set `true` at `:524`,
  which the rejection's `return` at `:521` never reaches, and set `false` **only** in
  `clear_world()` at `:312` (both write sites enumerated from the source, per rule 5 of §6.4).
  So a node whose *first* apply is rejected does read I = false — and for that node the silence
  is correct, while the user's actual problem in that scene is two world nodes in one scenario,
  which this ADR does not diagnose. But a node that applied successfully in one scenario and is
  then **re-applied into a scenario another live world already owns keeps I = `true`**, because
  nothing on the reject path clears it — and S has already moved to the rejected scenario
  (`:491-493`, written before the submit at `:516`). That state is #869; its two consequences,
  and the reason it is a product bug rather than something the predicate can close, are
  derived in full in the S write-site discussion above. It is recorded here as an accepted,
  bounded false negative rather than fixed, and §9 states why it is not a prerequisite the way
  #862 is.

  §8 records the measurement that has to confirm I latches at all in the editor, and the
  fallback if it does not.

**Why the count and not the assignment — the proof.** A valid-but-empty asset never reaches
the instance store, so it never steers the route. `InstanceStore::retain_asset()`
(`core/gaussian_splat_scene_director.cpp:722`) returns false when
`_populate_gaussian_data_from_asset()` fails (`:739-742`); that helper (`:638-653`) returns
false for a `ASSET_TYPE_DYNAMIC` asset because
`GaussianSplatAsset::populate_gaussian_data()` early-outs on `splat_count == 0`
(`core/gaussian_splat_asset.cpp:1655-1657`), and for every other asset type because
`get_gaussian_data()` `ERR_FAIL_COND_V`s on `splat_count == 0` and hands back a null Ref
(`:1548-1549`), which `:647-650` rejects. `register_instance()` early-returns on that false
at all three of its call sites (`:1150-1152`, `:1156-1158`, `:1175-1177`), so **no
`InstanceRecord` is appended at all**. With no record, the instance-hint loop at
`:2830-2846` finds nothing, the world submission's hint wins at `:2817-2827`, and the world
route renders normally. The repo has already *measured* this shape from the other direction:
the #798 round-3 comment at `nodes/gaussian_splat_node_3d.cpp:794-812` records a GPU run in
which "the director itself rejects the empty asset … so no submission survives". A warning
keyed on assignment would therefore tell a user that one of their two node types renders
nothing while their scene is, in fact, on screen — the false-positive shape this ADR exists
to avoid producing.

**This does not reopen the visibility question (§3.3).** The two cases are asymmetric, and
the asymmetry is in the store, not in the diagnostic:

- *Hidden* instance node: `register_instance()` still runs, the record is appended carrying
  `record.visible = false`, and the hint loop at
  `core/gaussian_splat_scene_director.cpp:2830-2846` filters only on
  `record.has_desired_residency_hint` — it never reads `visible`. The record still steers the
  route, the conflict is still real, and the warning must stay. §3.3 stands unchanged.
- *Empty* instance node: there is no record, so there is nothing to steer the route with.
- *Never-applied or cleared* world node: `_register_shared_renderer()` was never reached, or
  `_unregister_shared_renderer()` has already run (`nodes/gaussian_splat_world_3d.cpp:307`), so
  there is no `WorldSubmission` in the registry to lose the arbitration with. Same shape as the
  empty case: **no submission, no conflict.**

The predicate therefore still keys on submission rather than visibility. All that changed
across the revisions is that "this node submits" stopped being approximated — first by a
non-null `Ref` (P), then by a payload alone (I) — and started naming, conjunct by conjunct,
the guards the registration path actually applies.

**One accepted false negative, recorded rather than hidden.** If a node registers
successfully and its asset is emptied *afterwards*, `refresh_asset()` (`:773`) evicts the
cached payload at `:795` — `retain_asset()` does the same at `:760`, both via
`_evict_asset_data()` (`:681-690`) — but `register_instance()` returns on that false
**without removing the `InstanceRecord`** (`:1146-1148`). The record keeps
`has_desired_residency_hint`, so it still steers the route at `:2830-2846`, while every
buffer builder skips it for null data — which lands back on the `resident_no_instances` skip
of §3.1. In that transient state the conflict is real and this predicate stays silent.
Gating on director state instead would trade a rare, self-healing false negative for a
warning that blinks on every load (the director-state bullet above), which is the worse
trade. The underlying
defect — a record with no payload still steering the global route — is filed as follow-up
work in §9, next to the `visible` one; they are the same defect shape.

### 6.2 Warning text

Exact strings. Both name **both** class names and the observable consequence — and **neither
names a cause**, because §3.2 has not established one.

On `GaussianSplatWorld3D`:

```text
A GaussianSplatNode3D is registered in the same World3D as this node's content. The
renderer commits to a single render route per frame, so only one of the two node types
is drawn and the other renders nothing. Hiding either node does not restore the other.
Until per-submission routing lands (issue #788), keep GaussianSplatWorld3D content and
GaussianSplatNode3D content in separate World3Ds: put one under a SubViewport with its
own World3D, or run the two as separate scenes one at a time. Separating the World3Ds
is not enough on its own — this node keeps its content registered in the World3D it
last applied to, so re-apply it afterwards: reload the scene, or re-assign its
GaussianSplatWorld in the inspector. Splitting them into separate .tscn files that are
then instantiated under the same viewport does NOT help — they still resolve the same
World3D.
```

On `GaussianSplatNode3D`:

```text
A GaussianSplatWorld3D has content registered in this node's World3D. The renderer
commits to a single render route per frame, so only one of the two node types is drawn
and the other renders nothing. Hiding either node does not restore the other. Until
per-submission routing lands (issue #788), keep GaussianSplatNode3D content and
GaussianSplatWorld3D content in separate World3Ds: put one under a SubViewport with its
own World3D, or run the two as separate scenes one at a time. Separating the World3Ds
is not enough on its own — the GaussianSplatWorld3D keeps its content registered in the
World3D it last applied to, so re-apply that node afterwards: reload the scene, or
re-assign its GaussianSplatWorld in the inspector. Splitting them into separate .tscn
files that are then instantiated under the same viewport does NOT help — they still
resolve the same World3D.
```

**Why the workaround is phrased as a `World3D` separation and not as "separate scenes".**
The conflict is keyed by scenario RID (§6.1), and a scene file is not a scenario.
`Node3D::get_world_3d()` forwards to `Viewport::find_world_3d()`
(`scene/3d/node_3d.cpp:1054-1060`), which returns `own_world_3d`, else the viewport's
`world_3d`, else **recurses into the parent viewport** (`scene/main/viewport.cpp:4670-4681`).
Two `PackedScene`s instantiated under the same viewport therefore resolve the *same*
`World3D` and the same scenario, and the conflict is unchanged — so "put them in separate
scene files" on its own is not a workaround and must not be advertised as one. Only a
viewport that supplies its own world, or not having both scenes in the tree at the same
time, changes the key.

**Why the re-apply step is in the text, and what was verified about it (round 6).** The
previous revision told the user to separate the `World3D`s and stopped there. Separating them
by toggling `own_world_3d` on the viewport that contains the **world** node does not clear the
conflict: `Viewport::set_use_own_world_3d()` (`scene/main/viewport.cpp:4740`) never touches the
tree, and `GaussianSplatWorld3D::_notification()` (`nodes/gaussian_splat_world_3d.cpp:73-215`)
handles neither `NOTIFICATION_ENTER_WORLD` nor `NOTIFICATION_EXIT_WORLD` — its submission is
released only from `NOTIFICATION_EXIT_TREE` (`:126`) and `clear_world()` (`:307`). So the
submission stays registered in, and arbitrating over, the old scenario (#863), the conflict
stays live, and the warning correctly stays up. **Advice that fails when followed is a defect
in the deliverable**, so the text now names the step that finishes the job. That step was
verified by reading the path, not assumed:

- `apply_world()` (`:301`) → `_apply_world_internal()` (`:417`) → `_register_shared_renderer()`
  (`:452`), which resolves the **new** scenario at `:488` and republishes there.
- `submit_world_submission()` then migrates rather than duplicating: in phase 3 it re-resolves
  the owner's previous world with `_find_world_for_world_submission(owner_id)` — a scan over
  the whole `world_registry` keyed on owner (`core/gaussian_splat_scene_director.cpp:614-624`)
  — and when that world is not the one being committed to, it queues the renderer restore and
  calls `previous_world->submission_store.reset()` (`:2509-2517`). The stranded submission is
  released.
- The two user-reachable ways to reach `apply_world()` without a script: **re-assign the
  resource** — `set_world()` calls `apply_world()` whenever the node is in the tree
  (`:249-254`) — or **reload the scene**, which re-enters the node in the new world and
  re-registers from `NOTIFICATION_READY` (`:100-113`). Removing and re-adding the node works
  too: `EXIT_TREE` releases at `:126` and `ENTER_TREE` re-applies at `:96-98`, gated on
  `was_world_submission_active`, which a previously-applied node still carries.

The first sentence of each string changed for the same reason. "shares this node's `World3D`"
is **false** in exactly the state T10b constructs — after the switch the two nodes resolve
*different* `World3D`s while the conflict persists — and a warning that misstates the
configuration it is diagnosing is the failure mode §3.2 already made this ADR correct once.
"is registered in the same `World3D` as this node's content" is true in both the ordinary case
and the stranded one, because it names where the submission *is* rather than where the node
now resolves — the same distinction conjunct S makes (§6.1).

The phrase **"renders nothing"** and both literal class names are load-bearing: §6.4 asserts
on them, and the assertion is specified here rather than derived from the implementation
constant, so the test cannot become a tautology against the code it guards.

### 6.3 When the warning is re-evaluated

`get_configuration_warnings()` is pull-based — the editor calls it only after
`update_configuration_warnings()`, which does nothing itself except emit the tree's
`node_configuration_warning_changed` signal for the node (`scene/main/node.cpp:3498-3508`;
`TOOLS_ENABLED`-only, and it returns without emitting unless the tree has an edited-scene
root that is, or is an ancestor of, the node). A node must refresh **itself and its peers**,
because when node A enters the tree, node B's warning is the one that goes stale.

Add a small shared helper (e.g. `_notify_route_conflict_peers()`) that walks the *other*
group and calls `update_configuration_warnings()` on every peer that is inside the tree.

**The helper does not filter peers by scenario.** Scenario scoping is load-bearing in the
*condition* (§6.1) and only there. A refresh is idempotent — it asks a node to recompute,
it does not tell it an answer — so refreshing a peer in an unrelated `World3D` costs one
recompute and changes no result. Filtering here would be a micro-optimisation that has to
resolve a `World3D` at exactly the moments when the node cannot, which is how the first
draft of this section got the world switch wrong. Filtering by the *registered* scenario
(S, §6.1) would not have that problem — but it is not specified either, because it buys
nothing on an idempotent operation and no case in §6.4 can kill it, which would make it an
unkillable variation. §6.4's T10 and T10b are scoped to the resolved-scenario filter
accordingly.

**Ordering: four engine facts the refresh has to survive.** All four read at
`a04472a82cf`.

0. **For a `Node3D`, a tree notification *always* carries a world notification with it.**
   `Node3D::_notification` dispatches `NOTIFICATION_ENTER_WORLD` unconditionally from its
   `NOTIFICATION_ENTER_TREE` case (`scene/3d/node_3d.cpp:170`, inside the case opened at
   `:141`) and `NOTIFICATION_EXIT_WORLD` unconditionally from its `NOTIFICATION_EXIT_TREE`
   case (`:201`, inside the case opened at `:194`). Neither dispatch is conditional. Both our
   classes are `Node3D` subclasses, so **there is no tree transition without the matching
   world transition**, while the reverse does not hold — a `SubViewport` world switch sends
   the world notifications with no tree churn at all (fact 3). The world notifications are a
   strict superset. §6.3's trigger table is keyed on them alone, and the consequences for what
   is testable are worked through under "Triggers deliberately omitted" below.
1. **Entering: the group is registered before the notification.** `_propagate_enter_tree()`
   adds the node to its groups at `scene/main/node.cpp:337-339` and only then sends
   `NOTIFICATION_ENTER_TREE` at `:341` — which is where `ENTER_WORLD` is dispatched from
   (fact 0). A peer refreshed from ENTER_WORLD therefore already sees the entering node.
   Enter is safe as written.
2. **Leaving: the notification comes before the group is removed.** `_propagate_exit_tree()`
   sends `NOTIFICATION_EXIT_TREE` at `scene/main/node.cpp:412`, removes the node from its
   groups at `:424-427`, and nulls `data.tree` at `:436`. The `EXIT_TREE`-driven `EXIT_WORLD`
   is dispatched from *inside* that `:412` notification (fact 0), so it too lands before
   `:424-427`. A peer refreshed *during* either recomputes while the leaving node is still in
   the group and still reports `is_inside_tree()` — so it re-reports the conflict. The editor
   caches that answer, and the stale warning triangle this trigger exists to remove survives
   it.
3. **A `SubViewport` world switch sends EXIT_WORLD before it replaces the world, and the
   node cannot resolve a `World3D` while that notification runs.**
   `Viewport::set_use_own_world_3d()` calls `_propagate_exit_world_3d()` at
   `scene/main/viewport.cpp:4746-4748`, replaces `own_world_3d` at `:4750-4762`, and only
   then calls `_propagate_enter_world_3d()` at `:4764-4766`. That propagation dispatches
   `NOTIFICATION_EXIT_WORLD` **forward** (`viewport.cpp:4804-4812`), and forward means base
   class first (`core/object/object.h:477-482`), so `Node3D`'s own handler has already
   cleared `data.inside_world` at `scene/3d/node_3d.cpp:251` before any subclass handler
   runs. Two consequences: (a) from inside our EXIT_WORLD case `get_world_3d()` trips
   `ERR_FAIL_COND_V(!is_inside_world(), ...)`, returns null and prints an engine error
   (`scene/3d/node_3d.cpp:1054-1060`), so there is no scenario to filter peers by; (b)
   because the world has not been replaced yet, a peer refreshed *here* still resolves the
   **old** scenario, still sees this node in it, and keeps its warning. Afterwards
   ENTER_WORLD resolves the **new** world, so an enter-time walk scoped to the new scenario
   never revisits the old-world peer and it is never corrected.
   *(The EXIT_TREE-driven EXIT_WORLD is dispatched **backward** instead —
   `scene/3d/node_3d.cpp:201` passes `p_reversed = true` — so `inside_world` is still true
   on that path. The same notification has two orderings depending on who sent it; do not
   rely on either.)*

**Therefore: every refresh is deferred — on entry as well as on exit — and every world lookup
is guarded.**

- The helper issues *every* refresh, self and peer, as
  `callable_mp(node, &Node::update_configuration_warnings).call_deferred()`. A deferred call
  runs once the current propagation has finished — after group removal (fact 2) and after
  the world replacement *and* the matching `ENTER_WORLD` (fact 3) — so every node recomputes
  against the settled tree. `callable_mp` deferred calls are dropped when the target has
  been freed in the meantime, so a peer torn down in the same propagation is not a lifetime
  hazard.
- **Deferral is uniform, not exit-only, and that is a change from the previous revision.**
  Fact 1 shows an *immediate* enter-time refresh would also be correct, so the earlier
  exit-deferred/enter-immediate split was not wrong — it was two rules where one suffices, and
  the split is what forced §6.4 to try to attribute an immediacy mutation per trigger. One
  rule means one mutation ("issue any refresh immediately") with one place to apply it. It also
  removes a real ordering hazard the split still had: on a first entry the world node's
  `was_world_submission_active` (conjunct I, §6.1) is still `false` during `ENTER_WORLD`, and
  only becomes true at `NOTIFICATION_READY` (`nodes/gaussian_splat_world_3d.cpp:100-113`),
  which `Node::add_child()` runs synchronously after `_propagate_enter_tree()` and therefore
  still before the deferred flush. A deferred enter-time refresh reads I *after* `READY`
  latched it; an immediate one would read it before, publish "no conflict", and need `READY`
  added as a further trigger to correct itself. Uniform deferral makes that trigger
  unnecessary — see "Triggers deliberately omitted".
- Because the peer walk carries no scenario filter, the exiting node never has to resolve a
  world at exit time. That is what makes fact 3(a) harmless instead of a stream of engine
  errors in the editor log.
- **The condition does not resolve a world either.** Since round 4 it compares the *registered*
  scenario (conjunct S, §6.1) rather than `get_world_3d()->get_scenario()`, so fact 3(a) cannot
  reach it at all — not on self, not on any peer. R2 (`is_inside_world()`) survives for the
  separate reason given in §6.1: it excludes the instance node's unregistered
  `EXIT_WORLD`→`ENTER_WORLD` window, where S is stale-but-valid.

Call the helper, plus a deferred `update_configuration_warnings()` on self, from — and, per
"Triggers deliberately omitted" below, **only** from:

| Trigger | Conjunct it can flip | `GaussianSplatWorld3D` | `GaussianSplatNode3D` |
| --- | --- | --- | --- |
| `NOTIFICATION_ENTER_WORLD` / `NOTIFICATION_EXIT_WORLD` | R1/R2/S | **add both cases** (the class handles neither today — it has only `VISIBILITY_CHANGED` at `:207`). **Refresh only.** Making the world node *migrate* its submission on these notifications is #863 and is deliberately not specified here; the predicate is correct either way because S names where the submission is, not where the node now resolves | add (cases exist at `:445`, `:449`) |
| content assigned / applied / cleared | P and I | `set_world()` (`:249`, which applies when in-tree at `:252-254`), `apply_world()` (`:301`), `clear_world()` (`:306`) — the three sites that move `was_world_submission_active`. This class calls `update_configuration_warnings()` nowhere today, so both the self and the peer call are new | P only — the sites that already call `update_configuration_warnings()` (`:738`, `:827`, `:1063`, `:1283`, `:1293`, `:1303`, `:2969`); add the *peer* notification there |
| content **count** changes on an already-assigned resource | P | **new:** connect to the `GaussianSplatWorld` resource's `changed` signal and refresh self + peers, mirroring what the instance node already does for its asset. `GaussianSplatWorld::set_gaussian_data()` (`core/gaussian_splat_world.cpp:107`) emits it at `:120`, as do `set_chunk_payload_source()` (`:144`, emitting at `:158`) and `set_payload_metadata()` (`:206`, at `:214`). **Refresh only — this handler must not resubmit.** That the director keeps the apply-time payload `Ref` across such a change is #862; adding the resubmission is that issue's fix, with its own two-directional proof, and folding it in here would make a diagnostic PR change what the renderer draws. **The handler must refresh unconditionally, in both directions.** A `changed` that empties the payload matters as much as one that fills it, and the plausible optimisation — early-out when the new payload is empty, "there is nothing to warn about" — is a real mutation that T13 exists to kill | already covered on self: `set_splat_asset()` connects the asset's `changed` (`:730-733`) and `_on_asset_changed()` calls `update_configuration_warnings()` (`:2969`) — add the *peer* notification there. `GaussianSplatAsset::set_positions()` (`core/gaussian_splat_asset.cpp:876`) sets `splat_count` at `:885` and emits `changed` at `:895`; `set_splat_count()` (`:308`) emits at `:326` |

The last row exists **because** the predicate reads a count (P, §6.1) rather than a
non-null `Ref`. A resource that is assigned while empty and populated later — an import that
completes, a `set_positions()` call, a streamed world payload — flips the answer with no
assignment happening on the node, so assignment-only triggers would leave the warning
permanently wrong on exactly the scenes most likely to hit it. **Residual, accepted:** a
`GaussianSplatWorld` payload that grows without emitting `changed` leaves the warning stale
until the next world transition or reassignment. That is a quieter error than a warning
that blinks, and it is listed in §8 as unverified-by-run. T11, T12 and T13 (§6.4) are the cases
that make this row's wiring falsifiable rather than merely specified — T11 and T12 in the
empty→populated direction, **T13 in the populated→empty one, which is the direction round 6
found had no case at all**.

**Triggers deliberately omitted — each with the derivation that makes it omissible.** A
trigger that cannot change any conjunct, or that no test could ever attribute a failure to, is
not a safety margin: it is an unkillable duplicate, and §6.4 must not pretend otherwise.

- **`NOTIFICATION_ENTER_TREE` / `NOTIFICATION_EXIT_TREE` are omitted as strictly redundant.**
  By fact 0, `Node3D` dispatches `ENTER_WORLD` from `ENTER_TREE` (`scene/3d/node_3d.cpp:170`)
  and `EXIT_WORLD` from `EXIT_TREE` (`:201`), unconditionally. **There is therefore no trigger
  sequence in which a tree-keyed refresh fires and its world-keyed twin does not.** A
  tree-keyed refresh would be dead weight that no mutation could ever be attributed to,
  because deleting it always leaves the world-keyed refresh to satisfy the same assertion.
  **This is the defect the previous revision shipped:** it listed both rows, and then §6.4's
  T9 claimed "deleting the `EXIT_TREE` peer refresh" as a kill — which it is not, because
  removing a `Node3D` from the tree dispatches `EXIT_WORLD` as well and the surviving refresh
  keeps T9 green. Dropping the row is what makes T9's expectations satisfiable; §6.4 records
  the re-derivation.
- **`NOTIFICATION_READY` is omitted, and only uniform deferral makes that safe.** It is the
  site where conjunct I first becomes true for a serialized world node — `world` is
  deserialized before tree entry so `set_world()`'s `is_inside_tree()` guard (`:252`) skips the
  apply, `ENTER_TREE`'s re-apply is gated on the still-`false` flag (`:96-98`), and
  `auto_apply_on_ready` is not consulted until `READY` (`:108`). But `Node::add_child()` runs
  `_propagate_ready()` synchronously after `_propagate_enter_tree()`, so the `ENTER_WORLD`
  refresh — *deferred* — already recomputes after `READY` has latched I. Adding `READY` would
  make it a second unkillable duplicate of the kind above. **If the implementer reverts to an
  immediate enter-time refresh, this omission becomes a bug and `READY` must come back.**
- **The property setters that call `_resubmit_world_submission_if_registered()`**
  (`nodes/gaussian_splat_world_3d.cpp:261-299`) are omitted because they **cannot** flip I:
  that helper returns early unless the director already holds a submission for this node
  (`:410-412`), i.e. unless I is already true, and it never sets it false. **They can, however,
  rewrite S** — the helper reaches `_register_shared_renderer()` at `:414`, which writes
  `last_known_scenario` at `:491-493` — and round 8 records the bound rather than leaving "cannot
  flip I" to imply more than it says. In every state this ADR's cases construct the rewrite is to
  the value S already holds, so it changes no answer. It changes an answer in exactly one state:
  a node whose resolved and registered scenarios have come apart, which is #863's stranding. If
  the destination slot is free the setter genuinely *migrates* the submission (director phase 3,
  `core/gaussian_splat_scene_director.cpp:2509-2517`) and both scenarios' answers change with no
  refresh issued; if it is owned, the submission does not move but S does (#869). **Neither gets
  a trigger here**, deliberately: both states exist only because #863 is unfixed, both are
  cleared by that fix, and adding a trigger for them would be an unkillable duplicate by the
  standard of the two bullets above — no case in §6.4 reaches a rejected or migrating submit
  (see "the table is coupled to the predicate"). Recorded as a residual in §8.

**Two further triggers named in the brief are deliberately excluded, and each is pinned by a
test:**

- **Visibility change is not a trigger.** The condition is visibility-independent by design
  (§3.3): hiding a node does not restore the other, so a warning that disappeared on hide
  would be a lie. `NOTIFICATION_VISIBILITY_CHANGED` therefore stays untouched
  (`gaussian_splat_world_3d.cpp:207`, `gaussian_splat_node_3d.cpp:526`). Test T5 pins that
  the warning survives a `set_visible(false)` on either node. Note that neither conjunct P nor
  conjunct I (§6.1) softens this: a hidden node keeps its `InstanceRecord` and keeps steering
  the route, while an empty, never-applied or cleared node has no submission at all — the
  asymmetry is spelled out under §6.1's predicate and is what keeps "submission, not
  visibility" true rather than merely asserted.
- **`route_policy` change is not a trigger.** Per §3.4 the conflict exists at both values.
  Test T6 pins the warning at `route_policy = 0` and `= 1`.

If a later change makes the condition depend on either input, that change must also add the
trigger *and* flip the corresponding test — the tests are what make the omission deliberate
rather than forgotten.

### 6.4 Mutation proof

A configuration warning is exactly the kind of change that can be added and never fire. The
following cases go in `modules/gaussian_splatting/tests/test_gaussian_splat_node.h`, which
already has `[SceneTree]` warning coverage and the `ScopedTestNode` helper (see the existing
warning assertion at `:5436`).

**Build note for the implementer:** this build is compiled with `disable_exceptions=True`,
so `REQUIRE`/`CHECK` **do not abort**. Never write `REQUIRE(ptr)` followed by a dereference.
Use `if (!x) { FAIL("..."); return; }` at every early exit.

Shared helper for the assertions — a warning *counts as present* only if a single entry
contains all three of `"GaussianSplatWorld3D"`, `"GaussianSplatNode3D"` and
`"renders nothing"`. Substring-on-one-entry, not "any entry mentions the other class",
otherwise the pre-existing "No Gaussian splat asset..." warning could satisfy it.

Every case below names the mutation that must turn it RED. A control whose mutation is not
recorded is not a control — it is a case that happens to be green.

| # | Case | Setup | Assertion | Killed by (mutation that must turn it RED) |
| --- | --- | --- | --- | --- |
| **T1** | **the RED-without / GREEN-with case** | `GaussianSplatWorld3D` with a world holding ≥1 splat **and** `GaussianSplatNode3D` whose **`splat_asset` is a valid `GaussianSplatAsset` with ≥1 splat**, both under one `SceneTree` root, same `World3D`. **The `splat_asset` member is load-bearing:** §6.1's P resolves `splat_asset` first, so a T1 built on `renderer_data` instead would go RED under the `splat_asset.is_valid()` weakening and destroy that row's attribution | **both** nodes report the warning. `if (!found) { FAIL(...); return; }` | reverting either `get_configuration_warnings()` addition |
| **T2** | control — world alone | only the `GaussianSplatWorld3D`, carrying and having applied the **same ≥1-splat world as T1** | warning **absent** | making the condition "a GS node of *either* type is in this scenario", **counting the node itself** — see the Run B row, which is broad by construction |
| **T3** | control — instance alone | only the `GaussianSplatNode3D`, carrying the **same ≥1-splat runtime data as T1** | warning **absent** | same as T2 |
| **T4** | control — two instance nodes | two `GaussianSplatNode3D`s, **each carrying ≥1 splat and each resolving the same `World3D`**, no world node | warning **absent**. This is the control that stops "more than one GS node in the scene" from passing as an implementation; multiple instance nodes coexist correctly, they all publish `RESIDENT` | replacing the *other*-group lookup with a both-groups lookup |
| **T5** | control — visibility is irrelevant | T1 setup, then `set_visible(false)` on the instance node, then on the world node | warning **still present on *both* nodes in both configurations** (pins §6.3). Asserting both nodes is what makes the case indifferent to whether the mutation adds the conjunct to the self test or to the peer test | adding `&& is_visible_in_tree()` to the condition |
| **T6** | control — `route_policy` is irrelevant | T1 setup, run once with `route_policy = 0` and once with `= 1`, restoring the setting via the existing `ProjectSettingGuard` pattern (`tests/test_node_surface_cleanup.h:184`) | warning **present at both values** (pins §3.4/§6.3) | making the condition read `gs::settings::get_streaming_route_policy()`, in the polarity that agrees with the **project default** (`== GS_ROUTE_STREAMING`, §3.1) — the opposite polarity would take T1 down with it, see the Run B row |
| **T7** | control — different `World3D` | world node in the main tree, instance node inside a `SubViewport` with `own_world_3d = true` | warning **absent in both**. This is the control that proves the scenario scoping is real rather than a global "is there a world node anywhere" check | dropping the registered-scenario comparison (S, §6.1) from the condition |
| **T8** | control — no content at all | both node types present, neither has any resource assigned | warning **absent** — the user's actionable problem is "no asset", which the existing warning already states | **nothing — deliberately unkilled.** With no resource assigned the two nodes are excluded by **different** conjuncts, and neither by P alone. **Round-8 correction: this cell said both nodes fail "S, P and I at once", which round 6 had already refuted in the corrected derivation under "Deliberately unkilled" below — two incompatible explanations of the same row were left on the page.** The **world** node fails **P and I** only: `NOTIFICATION_READY` runs `_ensure_renderer()` (`nodes/gaussian_splat_world_3d.cpp:104`), which writes S at `:326-328` before any submission exists, and `auto_apply_on_ready` then reaches `_apply_world_internal()`, which sees a null `world`, calls `clear_world()` at `:423` and returns at `:424` — so `_register_shared_renderer()` is never entered at all (not, as this cell used to say, entered and returned at `:478-480`) and I is never set at `:524`. The **instance** node fails **S and P**: its `_register_shared_renderer()` returns at `nodes/gaussian_splat_node_3d.cpp:2897-2901` before it can reach `_register_instance_in_director()` at `:2903`, so the S write at `:2622-2625` never runs. Dropping P alone therefore still leaves each node excluded — **the world node by I, the instance node by S** — which is the same outcome the old cell claimed, reached by the correct route. T8 is kept as a **regression control** against the naive "two GS node types in one scene" implementation; the individually killable weakenings of P are T8b and T8c |
| **T8b** | control — **empty instance resource** | world node with ≥1 splat **and** instance node whose `splat_asset` is a valid `GaussianSplatAsset` with `get_splat_count() == 0` | warning **absent on both**, and the world node gains no other new warning. The world route renders in this configuration (§6.1 proof), so a warning here would be a lie | weakening the instance predicate to `splat_asset.is_valid()` — i.e. the "content assigned" spec this ADR shipped in round 1 |
| **T8c** | control — **empty world resource** | instance node with ≥1 splat of runtime data **and** world node whose `GaussianSplatWorld` has a null-or-zero-count `gaussian_data` *and* `chunk_payload_source` | warning **absent on both** | weakening the world payload test (P) to `get_world().is_valid()` |
| **T8d** | control — **world never applied** | T1 setup, except the world node has `auto_apply_on_ready = false` set *before* it enters the tree. Payload is fully renderable; `apply_world()` is never called (`nodes/gaussian_splat_world_3d.cpp:108-113`). **Setup constraint:** the `GaussianSplatWorld` must be assigned *before* tree entry — `set_world()` applies immediately when already in-tree (`:252-254`), which would make the world node active and T8d RED on the unmutated build | warning **absent on both** — the instance route renders in this configuration | **dropping conjunct I** — T8d shares that mutation with T8e. **Round-6 correction; this cell said "deliberately unkilled" for two rounds and was wrong.** The reasoning was that `apply_world()` never runs here, so `_register_shared_renderer()` is never reached and S is invalid as well as I false. S *is* valid: `NOTIFICATION_READY` calls `_ensure_renderer()` at `nodes/gaussian_splat_world_3d.cpp:104`, **before** the `auto_apply_on_ready` test at `:108`, and `_ensure_renderer()` writes `last_known_scenario` from the resolved world at `:326-328`. T8d's world node is therefore R1 ✓ R2 ✓ S ✓ P ✓ I ✗ — I is the sole false conjunct, exactly like T8e. **T8e is still not redundant:** it is the only case that also kills the `is_auto_apply_on_ready()` misreading of I, which T8d agrees with by accident (its flag is `false` and so is the real I) |
| **T8e** | control — **world explicitly cleared** | T1 setup (so the world *did* apply and the warning is present — assert that first), then `world_node->clear_world()`, then flush | warning **absent on both** afterwards, even though `get_world()` is still valid and still non-empty (`clear_world()` never nulls the `Ref`, `:306-317`) | dropping conjunct I — **shared with T8d since the round-6 S correction** (both are states with S valid and P true while I is false; here because `clear_world()` releases the submission but leaves the payload `Ref` and `last_known_scenario`, `:306-317`, S cleared only at `PREDELETE`, `:186`). **T8e's unique kill is implementing I as `is_auto_apply_on_ready()`** — the natural misreading of "apply intent", which reads `true` here and which T8d cannot catch |
| **T9** | the peer refresh on **tree exit** actually happens | T1 setup + the signal harness below, then `root->remove_child(instance_node)`, then flush | the world node appears in the recorder **and** *every* snapshot recorded for it no longer carries the conflict warning | (a) deleting the **instance** node's `EXIT_WORLD` peer refresh — now a real kill, because §6.3 no longer specifies an `EXIT_TREE` refresh to survive it; (b) issuing any refresh immediately instead of deferred |
| **T9b** | the peer refresh on **tree entry** actually happens | root already holding the T1 world node (applied, warning absent) + the signal harness, then `root->add_child(instance_node)` carrying ≥1 splat, then flush | the world node appears in the recorder **and** *every* snapshot recorded for it now **contains** the conflict warning | deleting the **instance** node's `ENTER_WORLD` peer refresh. **No immediacy mutation:** fact 1 makes an immediate enter-time refresh correct, so T9b cannot kill one and must not claim to |
| **T10** | the peer refresh survives a **`World3D` switch** | T1 setup with the instance node under a `SubViewport` that shares the main world (the default), + the signal harness, then `subviewport->set_use_own_world_3d(true)`, then flush | same as T9, for the world node left behind in the old world | (a) **scoping the peer walk by the *resolved* scenario (`get_world_3d()`)**, which must be the *resolved* one, see the derivation below; (b) **dropping the `registered_scenario(M) == registered_scenario(N)` equality**, which is the only conjunct that makes T10's expected end state clean. **Per-trigger deletion is not claimed, and — since round 5 — neither is the immediacy mutation:** both of T10's immediate snapshots are already clean, see the derivation below |
| **T10b** | the **opposite orientation**: the ***world*** node's viewport switches | T1 setup with the ***world*** node under a `SubViewport` that shares the main world (the default) and the instance node in the main tree, + the signal harness. Assert the T1 precondition (warning present on both), clear the recorder, then `subviewport->set_use_own_world_3d(true)`, then flush. **Setup constraint (round 6): this case must flush the message queue and must NOT call `tree->process()`.** The module test batches run headless, so `NOTIFICATION_READY` armed `set_process(true)` (`nodes/gaussian_splat_world_3d.cpp:118-120`) and one process tick would run `_notification_process()` → `_ensure_renderer()` (`:219-223`), which rewrites `last_known_scenario` from the **resolved** world (`:326-328`) while the submission stays stranded in the old scenario. That moves S off the scenario the submission is actually in, the peer match fails, and T10b fails unmutated | **both** nodes appear in the recorder **and** *every* snapshot recorded for each **still contains** the conflict warning. The world node's submission stays registered in — and stays arbitrating over — the old scenario (#863), so the conflict there is still live and the warning must not disappear | (a) **comparing `get_world_3d()->get_scenario()` instead of the registered scenario** — the world node resolves the new scenario, the peer match fails and both snapshots come back clean → RED. This is the round-3 predicate, and T10b is the only case that kills it; (b) deleting the `GaussianSplatWorld3D` `ENTER_WORLD`/`EXIT_WORLD` peer refresh — the instance node never moves and no other trigger fires, so the recorder is empty → RED; (c) **issuing any refresh immediately instead of deferred** — the switch dispatches `EXIT_WORLD` *forward* (`scene/main/viewport.cpp:4810`), so `Node3D` has already cleared `inside_world` (`scene/3d/node_3d.cpp:251`) when the world node's own handler runs; an immediate refresh there records a snapshot with R2 false on the node under test, i.e. **no** warning, while T10b's expected end state is the warning present → RED. This kill is new in round 5 and replaces the one T10 could not deliver; (d) **scoping the peer walk by the *resolved* scenario** — the world node's `EXIT_WORLD` walk resolves nothing (fact 3a) and its `ENTER_WORLD` walk resolves the *new* scenario, so the main-tree instance node is never reached and its entry is missing → RED. **No per-notification attribution** — see the derivation below |
| **T11** | the **world** resource-`changed` wiring is real | T8c setup (empty world resource, no warning) + the signal harness. Assert absence, clear the recorder, then `world_res->set_gaussian_data(<≥1 splat>)` (`core/gaussian_splat_world.cpp:107`, emits at `:120`), then flush | the recorder holds an entry for the **world node** *and* one for the **instance node**, and both snapshots now **contain** the conflict warning | (a) deleting the world node's `changed` connection — no entry at all; (b) keeping the connection but refreshing only self — the instance-node entry disappears while the world-node entry survives, which is why the peer half is asserted separately. **What T11 does not discriminate:** the state it constructs is the #862 divergence (§6.1's P bullet) — the director still holds the pre-swap empty `Ref`, so the warning it asserts is a recorded false positive until #862 lands. T11 passes identically before and after that fix, so it is a proof of the `changed` **wiring** only and must never be cited as evidence that the resubmission works |
| **T12** | the **instance** resource-`changed` wiring is real | T8b setup (empty `splat_asset`, no warning) + the signal harness. Assert absence, clear the recorder, then `asset->set_positions(<3 floats>)` (`core/gaussian_splat_asset.cpp:876`, sets `splat_count` at `:885`, emits at `:895`), then flush | same as T11, with the roles swapped | omitting the *peer* notification added to `_on_asset_changed()` (`nodes/gaussian_splat_node_3d.cpp:2965-2970`) — the world-node entry disappears. The self entry is pre-existing wiring (`:2969`); T12 additionally regression-pins it, but must not claim it as the new-wiring kill |
| **T13** | the **removal** direction of the world resource-`changed` wiring — **new in round 6** | T1 setup, with the constraint that the world's **sole** payload is `gaussian_data` (`chunk_payload_source` null), + the signal harness. Assert the T1 precondition (warning present on both), clear the recorder, then `world_res->set_gaussian_data(Ref<GaussianData>())` (`core/gaussian_splat_world.cpp:107`, assigns the null `Ref` at `:108`, skips the `is_valid()`-guarded metadata block at `:109-118`, emits `changed` at `:120`), then flush | the recorder holds an entry for the **world node** *and* one for the **instance node**, and *every* snapshot recorded for each now has the conflict warning **absent** | (a) deleting the world node's `changed` connection — no entry at all; (b) keeping it but refreshing only self — the instance-node entry disappears; (c) **the direction-asymmetric mutation T13 alone kills: early-out the `changed` handler when the new payload is empty** (`if (!payload(this)) { return; }` — the plausible "nothing to warn about, skip" optimisation). T11 stays GREEN under (c) because its new payload is non-empty; (d) **dropping conjunct P entirely** and (e) **weakening P on the world side to `get_world().is_valid()`** — under either, the world node still qualifies after the removal and both snapshots still carry the warning. **What T13 does not discriminate:** like T11 it sits in the #862 divergence, from the other side — the director still holds the pre-removal **non-empty** `Ref`, so `record_has_renderable_payload()` (`core/gaussian_splat_scene_director.cpp:843-851`) is still true and the submission is still steering while T13's asserted end state is "no warning". T13 passes identically before and after #862; the end state it asserts only becomes *true of the running system* once #862 lands, which is why the decision section makes that a prerequisite. It is a proof of the `changed` **wiring** and of P's removal direction, never of the resubmission |

**T9, T9b, T10, T10b, T11, T12 and T13 need a signal observer, not a getter.**
`get_configuration_warnings()` recomputes from the groups and from the live resources on every
call, so once the trigger has *fully completed* it returns the right answer whether or not any
refresh was ever issued. A case that only calls the getter therefore stays green with the
entire peer-notification mechanism deleted — a green test for a mechanism that is not there,
and the exact defect shape this ADR is trying to keep out of the module. **The same trap
applies to the resource-`changed` wiring**, which is why T11/T12/T13 use this harness rather than
reading warnings after mutating a resource: the predicate recomputes the count on demand, so
deleting the `changed` connection entirely leaves a getter-only case green. **T10b needs it
most of all:** its expected end state is *identical* to its start state, so without a recorder
it is green against a mutant that issues no refresh whatsoever. All seven cases must observe what
the editor observes:

1. `SceneTree::set_edited_scene_root(root)` (`scene/main/scene_tree.cpp:1623-1627`,
   `TOOLS_ENABLED`-only; the module test batches build under `target=editor tests=yes`).
   Without it `Node::update_configuration_warnings()` returns before emitting
   (`scene/main/node.cpp:3501-3506`) and no signal is observable at all. Save and restore the
   previous value.
2. Connect a recorder to the tree's `node_configuration_warning_changed` signal
   (`scene/main/scene_tree.cpp:1952`). For each emitting node the recorder stores **the
   result of calling `get_configuration_warnings()` on that node at signal time** — not just
   the fact that it fired.
3. Assert the case's **starting** state first — for T9/T10/T10b/T13 the T1 precondition (the
   conflict warning present on both nodes), for T9b/T11/T12 its absence — then clear the
   recorder, then perform the trigger, then `MessageQueue::get_singleton()->flush()` so the
   deferred refreshes of §6.3 run. **Flush only: none of the seven may call `tree->process()`**
   — see T10b's setup constraint for why one process tick is enough to move conjunct S under a
   headless run.
4. Assert: the recorder holds an entry for the peer node, **and *every* snapshot it recorded
   for that node** carries the expected end state (T9/T10/T13: no conflict warning;
   T9b/T10b/T11/T12: the conflict warning present). T10b/T11/T12/T13 assert this for **both**
   nodes — self and peer — because the two halves of the wiring fail separately.

Step 4 is what makes these cases discriminate, and each half kills a different mutation:

- **delete the peer `update_configuration_warnings()` call** → no entry for the peer → RED.
  A value-only assertion (round 1's T9) stays green here.
- **issue the peer refresh immediately instead of deferred** → an entry exists whose snapshot
  still carries the pre-trigger answer, because the refresh ran before group removal
  (§6.3 fact 2) or before the world replacement (§6.3 fact 3) → RED. A fires-at-all
  assertion stays green here.

**Why "*every* snapshot", not "the snapshot".** In T10 two refresh paths fire for the same
trigger (`EXIT_WORLD` then `ENTER_WORLD`, §6.3 fact 3), so the recorder can hold **two** entries
for the peer. Under the immediacy mutation the first is stale and the second is correct, and an
assertion phrased as "*an* entry whose snapshot is clean" is satisfied by the second — the
mutation survives. Quantifying over all entries is what makes immediacy killable at all when
two paths overlap. Under the specified implementation every refresh is deferred and every
snapshot is post-settle, so the stronger form costs nothing.

**What T9/T10/T10b do *not* prove, stated rather than papered over.** Per-trigger *deletion* is
**not attributable** for any of the world notifications, and this ADR no longer claims it:

- `EXIT_WORLD` alone cannot be isolated *by T10*, because `set_use_own_world_3d()` dispatches
  `ENTER_WORLD` immediately afterwards (`scene/main/viewport.cpp:4764-4766`) and its deferred
  refresh — unfiltered by scenario, by §6.3 — reaches the same old-world peer and satisfies
  the assertion. Symmetrically, deleting `ENTER_WORLD` leaves `EXIT_WORLD`'s deferred refresh
  to satisfy it. **The two mask each other by construction; no assertion available to T10
  separates them,** because both walk the same peer set and both settle to the same answer.
- The isolation is therefore obtained by **choosing triggers that fire one path**, not by
  inventing one the implementation cannot provide: **T9** (`remove_child`) reaches only
  `EXIT_WORLD` — now that §6.3 specifies no `EXIT_TREE` refresh — and **T9b** (`add_child`)
  reaches only `ENTER_WORLD`. Between them every deletion mutation of the previous revision's
  T9/T10 lists is killed, each by exactly one case.
- What is left for T10 is the **scenario-scoping** mutation: scope the peer walk by the
  *resolved* scenario and the `EXIT_WORLD` walk resolves no scenario at all (fact 3a) while the
  `ENTER_WORLD` walk resolves the *new* one and excludes the old-world peer — no entry, RED.
  **T10b is RED under that same mutation** (its own `EXIT_WORLD` walk resolves nothing and its
  `ENTER_WORLD` walk resolves the new own-world scenario, so the main-tree instance node is
  never reached), so the earlier "the only mutation T10 uniquely kills" was an overclaim and is
  withdrawn. The two are still both needed: they exercise the filter from the **two
  orientations**, which come apart the moment an implementation writes the walk twice instead of
  sharing `_notify_route_conflict_peers()`.
- **T10's second kill is the scenario *equality*.** Drop
  `registered_scenario(M) == registered_scenario(N)` and, after the switch, the migrated instance
  node still matches the world node, so the world node's post-flush snapshot still carries the
  warning where T10 expects it gone → RED. T7 kills the same mutation from a static setup; T10
  kills it across a transition. Neither is redundant with the other, because T7 never moves a
  node and cannot distinguish a condition that is right only until something migrates.
- **The *resolved* qualifier there is load-bearing since round 4, and the weaker claim is
  recorded rather than dropped.** A walk filtered by the **registered** scenario (S, §6.1)
  would *not* be killed by T10: S is latched, so the `EXIT_WORLD` walk resolves the old
  scenario fine, reaches the world node, and its deferred snapshot is correct. No case in this
  list kills that variant, and none is added, because it is not a defect — S-filtering is
  merely a micro-optimisation of an idempotent operation (§6.3). **T10 therefore kills the
  resolved-scenario filter and nothing weaker**, and the "Killed by" column says so.
- **T10b inherits the same limit on the other class, and claims no more.** Its trigger moves the
  *world* node's viewport, so both of that class's new world-notification refreshes fire and
  mask each other exactly as in T10; T10b's "Killed by (b)" is therefore the **pair** deleted
  together, not either one.
- **The immediacy mutation is killed by T9 and T10b — not by T10. This is the round-5
  correction, and it is the third unachievable expectation this section has shipped.** The
  derivation the previous revision did not do:
  - **T10 stays GREEN.** Its trigger moves the *instance* node, and T10 expects both of the
    world node's snapshots to be **clean**. On the exit leg, `_propagate_exit_world_3d()`
    dispatches `EXIT_WORLD` **forward** (`scene/main/viewport.cpp:4810`), so `Node3D` has
    already run `data.inside_world = false` (`scene/3d/node_3d.cpp:251`) before our handler:
    **R2 is false for the moving node**, so the peer's immediate recompute already finds no
    qualifying `M` and records a clean snapshot. On the enter leg,
    `_notification_enter_world()` (`nodes/gaussian_splat_node_3d.cpp:380-388`) has already
    re-registered through `_register_shared_renderer()` (`:383` → `:2903` →
    `:2622-2624`) by the time a refresh appended after it runs, so **S is already the new
    scenario** and the equality conjunct excludes it. Both immediate snapshots are clean, which
    is exactly what T10 asserts. Neither R2 nor S was re-checked against this row when round 4
    reshaped the predicate.
  - **T10b goes RED.** Its trigger moves the *world* node, and T10b expects every snapshot to
    **still contain** the warning. The same forward dispatch clears `inside_world` on the node
    under test itself, so an immediate refresh issued from the world node's `EXIT_WORLD`
    handler records R2 = false on **self** — a snapshot with no warning — and the "*every*
    snapshot" quantifier turns that transient into a failure. The asymmetry is entirely in the
    direction of the expectation: an immediate refresh in this window always under-reports, so
    only a case expecting the warning *present* can catch it.
  - **T9 stays RED**, and for the opposite engine reason: the `EXIT_TREE`-driven `EXIT_WORLD` is
    dispatched **reversed** (`scene/3d/node_3d.cpp:201`; `Node::_propagate_exit_tree()` also
    sends `EXIT_TREE` reversed at `scene/main/node.cpp:412`), so our handler runs *before*
    `Node3D` clears `inside_world`. The group is not removed until `node.cpp:424-427`,
    `data.tree` is not nulled until `:436`, and `_unregister_shared_renderer()`
    (`nodes/gaussian_splat_node_3d.cpp:2911`) never clears `last_known_scenario` (it moves only
    at `:2624` and `:519`). Every conjunct of the departing node still holds, so the immediate
    snapshot still carries the warning that the deferred one does not.
  - **The same notification, dispatched two ways, is what separates T9 from T10** — §6.3 fact 3
    already recorded that the orderings differ; what it did not do was carry the consequence
    into the "Killed by" column.
- **What T10b uniquely covers, and no earlier case does, is twofold.** (i) The
  registered-vs-resolved scenario mutation: T1, T7 and T10 all leave the two in agreement, so
  each of them stays green against a resolved-scenario condition; only T10b puts them in
  conflict. (ii) The world node's *own* world-notification refresh: T9 and T9b move the
  **instance** node and so exercise only that class's `EXIT_WORLD`/`ENTER_WORLD`, and T10 moves
  the instance node's viewport. Before T10b, deleting the `GaussianSplatWorld3D` half of the
  §6.3 trigger table's first row was an unkilled mutation.

**Spurious-fire control for all seven:** with the recorder connected and the same T1 setup,
remove an unrelated plain `Node3D` from the root and flush. The recorder must contain **no**
entry for either GS node. Without this, "the peer was signalled" could be satisfied by any
unrelated tree churn that happens to emit for the same node.

#### The table is coupled to the predicate — re-derive it whenever §6.1 changes

**This is the third unachievable expectation this section has shipped, and all three have the
same generator: a "Killed by" entry written by reasoning about what a mutation *ought* to break,
against a predicate that had since moved.**

- Round 1 mandated "T1 fails, T2–T9 pass", which no mutation could produce — reverting both
  overrides also kills T5 and T6.
- Round 2 replaced it with per-trigger Run B expectations that were equally unproducible,
  because `Node3D` dispatches `EXIT_WORLD` from its own `EXIT_TREE` handler
  (`scene/3d/node_3d.cpp:201`) and the two refreshes masked each other.
- Round 5 found the third: T10 under the immediate-refresh mutation, unachievable because R2 and
  S already exclude the moving node from both snapshots. Re-deriving the *whole* table against
  the current predicate — rather than only the reported row — found that round 4's
  registered-scenario S had silently invalidated **T8d** the same way (**round 6 found that
  this correction was itself wrong** — see below); that the "condition ignores content
  entirely" row (**T8**) had never been achievable, because I already excluded
  that state; and that **five** further rows had incomplete RED sets (see the Run B table). It
  also found four setups under-specified in ways that would have made a row vacuous rather than
  wrong: T1 must carry its content in `splat_asset` (not `renderer_data`) or the
  `splat_asset.is_valid()` row takes T1 down with T8b; T2, T3 and T4 must carry content at all,
  or their mutations cannot fire; and T8d must assign its world *before* tree entry, or
  `set_world()` applies it and T8d fails unmutated.

- **Round 6 found a fourth, with a *different* generator, and it is the more dangerous kind.**
  The previous three were attributions written against a predicate that had moved. This one was
  written against a **misreading of the code the predicate mirrors**: rounds 4 and 5 both stated
  that `last_known_scenario` is written "only inside `_register_shared_renderer()`", and
  `_ensure_renderer()` (`nodes/gaussian_splat_world_3d.cpp:326-328`) writes it too — from
  `NOTIFICATION_READY` (`:104`), before any submission exists. Three consequences, none of which
  a re-derivation against the *stated* predicate would have caught, because the stated predicate
  was fine and its description of the code was not:
  - **T8d** is killable after all (S is valid there), so it comes off the deliberately-unkilled
    list it was added to in round 5;
  - **"drop conjunct P entirely"** stops being an unkilled row: T8b, T8c and the new T13 are all
    states in which P is the sole false conjunct. Round 5 checked that row against T8 only, and
    T8 was the one case in the list where it does *not* work — even though the same round's Run B
    table already stated, in the "weaken P on the world side" row, that T8c has "S valid, I true,
    and P the only false conjunct". **The contradiction was on the page and nobody read the two
    rows against each other;**
  - a headless process tick can move S without moving the submission, which is a setup
    constraint on T10b rather than a table correction.

- **Round 8: one stale row, and a full re-derivation that changed no expectation — recorded
  because "nothing changed" is a result only if the check was actually run.** The stale row was
  **T8**, whose "Killed by" cell still carried round 5's "fails S, P and I at once" reading
  after round 6 had corrected the same claim two sections lower. The expected outcome was
  unaffected, and that is precisely why it survived a round: **the generator here is neither a
  moved predicate nor a misread of the code, but a correction applied at one of the two places
  that stated the same fact.** Rule 6 below is the response.
  Round 8 also re-derived every row against the #869 finding (S can outlive a rejected submit,
  §6.1) and against the corrected account of I under arbitration rejection. **No row's setup,
  assertion or RED set changes**, and the reason is single and checkable rather than
  row-by-row optimism: **no case in this list ever drives a `submit_world_submission()` that
  the director rejects.** T1–T8e stand up one world node per scenario, so arbitration never
  fires; T9/T9b/T10 move the *instance* node, which does not arbitrate a world slot at all;
  T10b moves the world node's viewport but §6.3 specifies its new `ENTER_WORLD`/`EXIT_WORLD`
  cases as **refresh-only**, so no re-apply and no submit occurs; T11/T12/T13 drive the
  resource `changed` handler, specified refresh-only for the same reason. **The arbitration
  state is therefore deliberately not covered by any case**, and adding one is rejected on the
  ADR's own rules: its correct end state only becomes true of the running system after #869,
  so a case written now would assert an outcome the build cannot produce — the fourth
  unachievable expectation, written knowingly. It is recorded as a bounded false negative in
  §6.1 and §8 instead, which is the same disposition #862's and #863's divergences already
  have in T11 and T10b.

**The rules this leaves behind.**

1. **Before writing a "Killed by" entry, enumerate which notifications the trigger actually
   dispatches and which refresh paths survive the mutation.** If two paths reach the same
   assertion, either pick a trigger that reaches one, or drop the attribution. Do not write the
   transcript you expect to see.
2. **Enumerate the trigger in both orientations** (round 4's corollary, which produced T10b).
   T10 moved one of the two node types and the case list silently assumed the other was
   symmetric; it is not, because only one of the two classes migrates its registration on a
   world switch (§6.1, #863).
3. **The mutation table is a function of the predicate in §6.1, not an independent artifact.**
   Every "Killed by" entry is the claim "*this conjunct is the one that changes value in this
   state*". Adding, removing or reshaping a conjunct therefore invalidates rows that were
   correct against the previous predicate — silently, because the case list does not mention the
   conjuncts by name. **Any change to §6.1's predicate, to §6.3's trigger set, or to the
   deferral rule requires re-deriving §6.4 in full, row by row, and re-checking every case
   against every mutation — not only the rows that mention the changed conjunct.** Round 4
   changed S and re-derived T10b alone; two rows it did not look at (T8d, T10) were wrong by the
   end of that round.
4. **A row whose reason cannot be written is not a row.** State, per row, *which conjunct or
   which snapshot observes the difference*. If no single-conjunct mutation reaches a case, list
   the case as **deliberately unkilled** with that reason. An honest "not independently
   killable" is worth more than an attribution the next round deletes — T8 and T8d are now on
   that list, and the list is expected to grow, not shrink. **Round 6 shrank it anyway** — T8d
   came off — which is the reminder that "deliberately unkilled" is a claim about the code, not
   a retirement: it has to be re-checked like any other, and it decays in the direction of
   *hiding* a missing test.
5. **Enumerate the write sites of any latched field the predicate reads, from the source, and
   put the list in §6.1.** (Round 6's rule.) Conjuncts I and S are both latched fields, and
   three rows of this table are claims about *when a field does not move*. Rounds 4 and 5 wrote
   those claims from the function they expected to own the field rather than from a grep, and
   the field had a second writer. A conjunct-by-conjunct re-derivation cannot catch this,
   because the predicate reads correctly and only its mirror-of-the-code column is wrong — so
   the enumeration has to be explicit, and it has to be re-run whenever a "Killed by" entry
   depends on a field *not* having been written.
6. **When a correction lands, grep for every other place that states the corrected fact and fix
   them in the same edit.** (Round 8's rule.) Round 6 corrected the "S is invalid in the
   never-applied state" misreading in §6.1, in T8d and in the deliberately-unkilled derivation,
   and left the *same* claim standing in T8's "Killed by" cell — where it survived a round
   because the row's expected outcome was unchanged by it. **An outcome that is still right is
   not evidence the reasoning is:** a mutation table whose rows are read as attributions is
   wrong the moment two rows explain the same state incompatibly, whatever the expectations say.
   The check is mechanical — grep the corrected claim, not the corrected row.

#### The mutation runs — what they must actually produce

**The mutation runs are part of the deliverable, not optional.** Two runs are mandated, and
the expected outcomes below were derived from the case list above rather than assumed.

**Run A — the broad mutation.** Revert both `get_configuration_warnings()` additions and
keep everything else (groups, helper, triggers, deferral). No conflict warning is producible
at all, so every case that *positively requires* the warning fails and every case that
requires its *absence* passes:

> expected: **T1, T5, T6, T8e, T9, T9b, T10, T10b, T11, T12, T13 FAIL —
> T2, T3, T4, T7, T8, T8b, T8c, T8d PASS.**

Derived case by case, not assumed. T5 and T6 fail because each positively asserts the same
warning T1 does, under a perturbation. T8e fails on its *first* assertion — it must observe
the warning present before `clear_world()` in order for its absence afterwards to mean
anything — while T8d, which asserts absence throughout, passes. T9 and T10 fail on their T1
precondition (step 3 above), which they must assert, because a T9 that skipped it would be
green against a mutant that produces no warning at all, i.e. vacuous. T10b fails for the same
reason — it asserts the T1 precondition too, and its post-condition is also positive, so it
fails at both ends. **T13 fails on its T1 precondition alone:** its post-condition is the
*absence* of the warning, which a warning-less build satisfies, so it is the one case in this
run whose failure is entirely at the front. T9b, T11 and T12 pass
their absence precondition and then fail on the positive post-condition: the signals still
fire (helper and triggers are intact in Run A) but no snapshot ever contains a warning that no
longer exists.

**A transcript reading "T1 fails and everything else passes" is not achievable for this
mutation and must not be written.** If a run does produce it, the assertions are matching
something other than the new warning — most likely the pre-existing "No Gaussian splat asset…"
string — and the whole suite proves nothing; the three-substring helper above exists to
prevent exactly that.

**Run B — the narrow mutations, one per distinct mutation.** Run A proves the warning exists;
it proves nothing about *which* case guards *what*, because a mutation that kills ten cases at
once cannot attribute any of them. Apply each **distinct** mutation named in the "Killed by"
column independently and record, for each, **the complete RED set the row names** — not merely
that the designated case went RED. A run that produces more REDs than the row lists is a
finding about this table, not a passing result.

**The `Expected RED` column is complete, not indicative**, and the third column states *why each
listed case observes the mutation*. A row whose third column cannot be written is not a row —
see rule 4 above. `T1 GREEN` is implied by every row except the first and is called out only
where the mutation could plausibly take it down. **The first row is the one deliberate
exception:** it is a broad, Run-A-class mutation kept because T2 and T3 have no narrow kill, and
it attributes nothing on its own.

| Mutation | Expected RED (complete) | Which conjunct or snapshot observes it |
| --- | --- | --- |
| condition = "a GS node of either type is in this scenario", **counting the node itself** — **broad, Run-A class; not an attribution row** | T2, T3, T4, T7, T8b, T8c, T8d, T8e, T9, T9b, T10, T11, T12, T13 — only T1, T5, T6, T8 and T10b GREEN | Counting self makes the `∃M` clause vacuously true, so the condition collapses to `WOULD_STEER(N)` and **every** qualifying node warns. That takes down every case asserting absence (T2/T3/T4/T7/T8b/T8c/T8d/T8e), every absence *precondition* (T9b/T11/T12) and every clean *post*-condition (T9/T10/T13 — in T13 the world node stops qualifying but the instance node still does, and under the mutant it warns about itself). T8 survives because neither of its nodes qualifies even with the peer clause gone — its world node fails P and I, its instance node fails S and P (round-6 reading; see the deliberately-unkilled list); T10b, T5 and T6 assert presence throughout. **T2 and T3 have no narrow kill and this row does not pretend otherwise:** the only way to break "a lone node must not warn" is to stop requiring a peer, and any mutation that does so reaches every one-qualifying-node control at once. Run it, but attribute nothing from it beyond "the peer requirement exists" |
| other-group lookup → both-groups lookup, **self still excluded** | T4 | T4 is the only case with two *same-type* nodes that both satisfy WOULD_STEER in one scenario. Its setup must therefore give both instance nodes ≥1 splat, or the mutation cannot fire and the case is vacuous |
| add `&& is_visible_in_tree()` to the condition | T5 | T5 is the only case that falsifies the added conjunct while leaving every real conjunct true. It asserts on **both** nodes in each configuration so the kill lands whether the mutation is applied to the self test, the peer test, or both |
| condition reads `get_streaming_route_policy()`, **in the polarity that agrees with the project default** (`== GS_ROUTE_STREAMING`; default is `1`, §3.1) | T6 | T6 is the only case that runs one setup at both policy values; the added conjunct is false at `route_policy = 0`. **The polarity is load-bearing:** `== GS_ROUTE_RESIDENT` would be false at the default and take T1, T5, T9–T12 down with it, destroying the attribution |
| drop the `registered_scenario(M) == registered_scenario(N)` equality | T7 **and** T10 | T7 — its two nodes are in different scenarios from the start, so the equality is the only conjunct excluding them. T10 — after the switch the mover's S is the *new* scenario, and the equality is precisely what makes T10's expected end state clean; without it the post-flush snapshot still warns. T7 pins it statically, T10 across a transition; a condition that is right until something migrates passes T7 |
| compare `get_world_3d()->get_scenario()` instead of the registered scenario | T10b **only** | Only T10b puts registered and resolved in conflict — the world node resolves the new scenario while its submission stays registered in the old one (#863). T1, T7, T9, T9b, T10, T11 and T12 all leave the two in agreement and stay GREEN |
| drop conjunct P entirely | T8b, T8c, T11, T12 **and** T13 — **round-6 correction; this row said "nothing — deliberately unkilled"** | Three states in the list have P as their *sole* false conjunct, and round 5 checked the row against the one state that does not. **T8b** — its instance node holds a valid empty `splat_asset`, so `_register_instance_in_director()` clears its asset-null early-return at `nodes/gaussian_splat_node_3d.cpp:2607-2609` and **writes S at `:2622-2625`** before `register_instance()` declines to append a record; S valid, I constant-true, P alone false. **T8c** — the world node's empty-but-assigned `GaussianSplatWorld` is still accepted, so S is valid and I is true (the same fact the "weaken P on the world side" row already stated). **T13** — after the payload removal the world node is R1 ✓ R2 ✓ S ✓ I ✓ P ✗. **T11 and T12** fail on the absence *preconditions* they inherit from T8c and T8b. GREEN: **T8** (its world node also has I false, and its instance node never reaches the S write — it stops one function earlier, at `_register_shared_renderer()`'s no-data return `:2897-2901`, so the asset-null guard at `:2607-2609` is not even reached), **T8d** and **T8e** (I false), **T7** (scenario equality) |
| weaken P on the instance side to `splat_asset.is_valid()` | T8b **and** T12 | T8b is built for it. T12 inherits T8b's setup and asserts the warning's **absence** as its step-3 precondition, so it fails before its trigger runs |
| weaken P on the world side to `get_world().is_valid()` | T8c, T11 **and** T13 | T11 inherits T8c's setup and its absence precondition. Both depend on an assigned-but-empty `GaussianSplatWorld` still being **accepted** — `submit_world_submission()` rejects only on arbitration (`core/gaussian_splat_scene_director.cpp:2390-2399`), never on an empty payload — so I is true, S is valid, and P is the only false conjunct. **T13 reaches the same state from the other direction:** `clear_world()` is never called, so `get_world()` is still valid after `set_gaussian_data(null)` and the mutant keeps the world node qualifying where T13 requires it to stop |
| drop conjunct I | T8d **and** T8e | Both are states with S valid and P true while I is false — T8d because `NOTIFICATION_READY` writes S through `_ensure_renderer()` (`nodes/gaussian_splat_world_3d.cpp:104` → `:326-328`) before the `auto_apply_on_ready` test at `:108` skips the apply; T8e because `clear_world()` releases the submission but leaves the payload `Ref` and `last_known_scenario` (`:306-317`). **T8d's listing here is the round-6 correction** — round 5 moved it to the deliberately-unkilled list on the incorrect reading that S is invalid in the never-applied state |
| implement I as `is_auto_apply_on_ready()` | T8e **only** | `auto_apply_on_ready` is still `true` after `clear_world()`, so the mutant reads true where the real I reads false. T8d stays GREEN because its flag is `false` and the mutant agrees with the real answer by accident — which is exactly why T8d cannot substitute for T8e, even now that both kill the plain I-drop |
| delete the **instance** node's `EXIT_WORLD` peer refresh | T9 | `remove_child()` reaches `EXIT_WORLD` and nothing else that refreshes (§6.3 specifies no `EXIT_TREE` refresh), so the recorder holds **no entry** for the world node. T10 stays GREEN: its trigger also dispatches `ENTER_WORLD`, whose unfiltered walk reaches the same peer |
| delete the **instance** node's `ENTER_WORLD` peer refresh | T9b | `add_child()` reaches `ENTER_WORLD` only, and T9b's node already carries its content *before* entry, so no content-assignment trigger masks the deletion → no entry for the world node. T10 stays GREEN for the mirror-image reason |
| delete the **world** node's `EXIT_WORLD` **and** `ENTER_WORLD` peer refreshes (the pair — they mask each other, see the derivation) | T10b | In T10b only the world node moves, so these are the only refreshes that fire at all; with both gone the **instance** node has no entry. T10b asserts an entry for *both* nodes, which is what makes the missing one a failure |
| issue refreshes immediately instead of deferred | T9 **and T10b** — **T10 stays GREEN** | T9 — the `EXIT_TREE`-driven `EXIT_WORLD` is dispatched *reversed* (`scene/3d/node_3d.cpp:201`), so every conjunct of the departing node still holds when an immediate refresh runs and the snapshot still carries the warning T9 expects gone. T10b — the world-switch `EXIT_WORLD` is dispatched *forward* (`scene/main/viewport.cpp:4810`), so `Node3D` has already cleared `inside_world` (`node_3d.cpp:251`) and an immediate self-refresh records R2 = false, i.e. **no** warning, where T10b expects it present. T10 cannot kill it because *both* of its immediate snapshots are already clean — R2 excludes the mover on the exit leg and S on the enter leg — and clean is what T10 asserts. Full derivation above |
| scope the peer walk by the **resolved** scenario (`get_world_3d()`) | T10 **and** T10b | T10 — the mover's `EXIT_WORLD` walk resolves no scenario at all (§6.3 fact 3a) and its `ENTER_WORLD` walk resolves the *new* one, so the old-world peer is never reached. T10b — the same structure on the other class: the world node's `EXIT_WORLD` walk resolves nothing and its `ENTER_WORLD` walk resolves the new own-world scenario, so the main-tree instance node is never reached. Both are kept: they exercise the filter from the two orientations, which come apart if the walk is written twice instead of shared |
| delete the world resource `changed` connection | T11 **and** T13 | No refresh of any kind fires, so the recorder is empty and neither case's post-condition has anything to read. The getter would still return the right answer in both, which is why both use the recorder |
| refresh only self from the world `changed` handler | T11 **and** T13 | The world-node entry survives and the **instance**-node entry disappears; both cases assert the two halves separately for exactly this |
| early-out the world `changed` handler when the new payload is empty (`if (!payload(this)) return;`) | T13 **only** | The mutation is direction-asymmetric and T13 is the only case that drives the `changed` signal in the emptying direction. T11 and T12 both populate, so their handler still runs and they stay GREEN. This is the row T13 was added for |
| omit the peer notification in `_on_asset_changed()` | T12 | Mirror of the row above on the instance side: the world-node entry disappears. The *self* entry at `nodes/gaussian_splat_node_3d.cpp:2969` is pre-existing wiring and T12 must not claim it as the new-wiring kill |

**Deliberately unkilled — cases no single-conjunct mutation reaches.** Listing them is a
result, not an omission (rule 4 above):

- **T8** (no resource assigned to either node), and it is now the **only** entry on this list.
  Its two nodes are excluded by different conjuncts, and neither by P alone:
  - the **world** node fails **P and I** (S is valid, because `NOTIFICATION_READY` runs
    `_ensure_renderer()` at `nodes/gaussian_splat_world_3d.cpp:104` and that writes
    `last_known_scenario` at `:326-328` regardless of whether a submission follows;
    `auto_apply_on_ready` then reaches `_apply_world_internal()`, which sees a null `world`
    and calls `clear_world()` at `:423`, so I is never set at `:524`);
  - the **instance** node fails **S and P**: with `splat_asset`, `renderer_data` and
    `runtime_asset` all null, `_register_shared_renderer()` returns at
    `nodes/gaussian_splat_node_3d.cpp:2897-2901`, and `_register_instance_in_director()`
    would in any case return at its asset-null check `:2607-2609`, which precedes the S write
    at `:2622-2625`.

  So every single-conjunct mutation leaves at least one node excluded on both sides, and only a
  composite mutation reaches the case — which attributes nothing. T8 is retained as a
  **regression control** against the naive "two GS node types in one scene" implementation.
  *(Round 5 put it here for the right outcome and the wrong reason — "fails S, P and I at once"
  — which round 6 corrected to P-and-I on the world node. The conclusion is unchanged.)*

**Five entries are deliberately absent** (round 5 counted six; one has since become a row). Round-3 corrections: no "delete the `EXIT_TREE` peer
refresh" row (§6.3 no longer specifies one), and no row attributing a deletion to `EXIT_WORLD`
*versus* `ENTER_WORLD` within T10 (the derivation above shows no case can). Round-4: the same
non-attribution holds on the world node's side, which is why T10b's deletion row names the
**pair**; and there is no "scope the peer walk by the *registered* scenario" row, because that
variation is not a defect and no case kills it — the row above is deliberately restricted to the
*resolved*-scenario filter. Round-5: there is no row for **dropping S's validity conjunct** (as
opposed to the S *equality*, which T7/T10 kill), because the only case in the list holding an
invalid S is T8's instance node, whose *peer* holds a valid one — so the equality excludes the
pair anyway and dropping the validity test changes no answer. **Round-6: the fifth entry, "drop
conjunct P alone", is no longer absent — it is a row, killed by T8b/T8c/T11/T12/T13.** Round 5
listed it here after checking it against T8 only; T8 is the single case in the list where P is
*not* the sole false conjunct. Each mutation is a one-line result and none requires rebuilding
more than the two node translation units.

Paste both transcripts into the PR; do not describe them.

## 7. Consequences

- **Immediately:** the conflict becomes visible at author time on both node types, on a
  scene the user is editing, naming the consequence and a workaround. No render behaviour
  changes; no existing scene stops working.
- **The warning is a stopgap with an expiry condition.** It is deleted, not amended, when
  Option 2 lands. Both the warning text and this ADR name #788 so the removal is findable.
- **`qa_stream_multi_asset.tscn` stays quarantined** (`qa_test_runner.gd:53-55`). Option 1
  does not change what the runtime can prove; un-quarantining it is Option 2's job, and
  doing it earlier would re-create the #785 defect class.
- **Nothing in the repo exercises coexistence after #854 merges.** That is the correct state
  for now — a scene that cannot work should not be gating — but it means the first Option 2
  slice must bring its own coverage rather than inheriting any.
- **Blocked on #862.** The warning is the right decision and is not implementable correctly
  until that lands (Status block, §5.1, §6.1). This is the only consequence in this list that
  has a date attached to it.
- **Cost:** an editor-only code path in two node classes, one public const accessor on
  `GaussianSplatWorld3D` (§6.1, conjunct I), plus nineteen test cases (T1–T13 with
  T8b/T8c/T8d/T8e, T9b and T10b) and the spurious-fire control. The peer notification is the only
  non-trivial part, and it runs on world transitions, resource `changed` and content
  assignment — never per frame. Every refresh is deferred by one idle frame (§6.3), which is
  invisible in the editor and is why T9/T9b/T10/T11/T12/T13 flush the message queue.

## 8. What is not verified here

This ADR was written under a documentation-only constraint: **no build was run**, so nothing
below was executed.

- **The static reading of the route decision (§3) is verified by source citation only.** The
  runtime attribution — the reason string, the blank captures, the symmetry under
  `route_policy` — is #785's and #788's measured evidence, reproduced here as reported, not
  re-measured.
- **Unresolved (§3.2): why the world submission failed the
  `is_active() && record_has_renderable_payload()` test** in the reported run. §3.2
  enumerates the candidate states — an arbitration-rejected submission, a record reset by
  release/eviction/teardown, a payload that is never renderable, or a load-timing race — and
  the recorded reason string is consistent with **all** of them. **No measurement in this
  repo distinguishes them, so neither this ADR nor the §6.2 warning attributes the outcome
  to any one cause, and in particular neither claims load timing.** The capture that would
  settle it: run the mixed scene with `is_active()`, `record_has_renderable_payload()` and
  `has_desired_residency_hint` logged from inside
  `get_submission_residency_hint_for_renderer()` (`core/gaussian_splat_scene_director.cpp:2811`)
  on every frame from the first frame to steady state — a gate that fails early and passes
  later is timing; a gate that never passes is state. That run needs a build and was not
  performed here. It does not change this decision (the conflict exists either way, §3.4)
  but it does determine whether Option 2 also has to fix a startup ordering race.
- **The Option 1 spec compiles/behaves as written is unproven.** Group registration from a
  constructor, `get_world_3d()` validity inside the editor's edited-scene viewport, and the
  peer-notification ordering are all read from source, not run.
- **Whether conjunct I (§6.1) ever latches at author time is the single highest-risk
  unverified item, because if it does not the warning never appears in the editor at all.**
  `was_world_submission_active` becomes true only inside `_register_shared_renderer()`
  (`nodes/gaussian_splat_world_3d.cpp:524`) and only after `submit_world_submission()` accepted
  the submission. That an edited-scene `GaussianSplatWorld3D` reaches `NOTIFICATION_READY`,
  finds a `GaussianSplatSceneDirector::get_singleton()`, and gets its submission accepted **in
  the editor** is read from source, not run. **The measurement:** open a scene holding an
  applied world node in the editor and log the flag from `get_configuration_warnings()`.
  **The fallback if it stays false:** replace the field with a dedicated author-side intent
  flag set at the same three sites — `set_world()` with a payload, `apply_world()`,
  `auto_apply_on_ready` at `READY` — and cleared by `clear_world()`, *without* the
  director-acceptance dependency. That is strictly author-time, removes the
  world-vs-world-arbitration caveat recorded in §6.1, and costs one bool. It is not specified
  as the primary because it adds state that duplicates an existing field; the measurement
  decides.
- **The §6.3 ordering facts are read, not executed.** All four engine orderings (the
  tree→world dispatch of fact 0, group registration vs. `ENTER_TREE`, `EXIT_TREE` vs. group
  removal, and `EXIT_WORLD` vs. the `own_world_3d` replacement) are cited to line, but the
  *consequence* claimed for the fix —
  that a `call_deferred()` peer refresh issued from `EXIT_WORLD` lands after
  `_propagate_enter_world_3d()` and the `viewport_set_scenario()` at
  `scene/main/viewport.cpp:4768-4770` — was not observed running. T10 is the case that
  measures it; if it does not, the alternative is to capture the old-world peer set at exit
  and refresh that captured set, which is the same fix with explicit bookkeeping.
- **Conjunct S (§6.1) is read from source, not run — but round 6 removed its dependency on
  conjunct I's measurement, on the world node.** The previous revision said S "is written only
  inside the registration paths", so a director that never accepts an editor submission would
  leave S `RID()` and the warning would never appear. That is still true of the **instance**
  node (`nodes/gaussian_splat_node_3d.cpp:2622-2625`, reachable only through
  `_register_instance_in_director()`). It is **not** true of the world node:
  `NOTIFICATION_READY` calls `_ensure_renderer()` (`nodes/gaussian_splat_world_3d.cpp:104`),
  which writes `last_known_scenario` from `get_world_3d()` at `:326-328` with no director
  involvement at all. So on that class S is author-side already, and if I's fallback (an
  author-side intent flag) is taken, S needs **no** matching fallback there. What remains
  unverified is only that an edited-scene node reaches `READY` and resolves a valid `World3D`
  in the editor — a weaker claim than the one this bullet used to make, and it is settled by
  the same editor run as the I bullet.
- **The #869 false negative is derived from source, not run, and it is a recorded residual
  rather than a covered case.** That a re-apply into a scenario owned by another live world
  node leaves S on the rejected scenario while I stays `true` (§6.1) is read from
  `nodes/gaussian_splat_world_3d.cpp:491-493`/`:516-524` and
  `core/gaussian_splat_scene_director.cpp:2392-2401`/`:2509-2517`. No case in §6.4 constructs
  it, by the derivation recorded there, so **nothing in the mutation suite would notice if this
  reading were wrong in either direction.** The same applies to the migration variant in §6.3's
  omitted-triggers list — a renderer-parity setter moving the submission between scenarios with
  no refresh issued. Both are cleared by #869 and #863 respectively; until then they are the
  two states in which this warning is knowingly not correct, and neither is reachable in the
  one-world-node scene the warning targets.
- **T10b's scenario arithmetic is read, not executed.** That a `SubViewport` which does *not*
  own its world resolves the *same* scenario RID as the main tree
  (`Viewport::find_world_3d()` recursing into the parent viewport,
  `scene/main/viewport.cpp:4670-4681`), and that `set_use_own_world_3d(true)` then yields a
  different one, is the precondition for T10b discriminating anything at all. If a headless
  `[SceneTree]` does not allocate distinct scenarios, T10b (and T7, which shares the
  assumption) must move to a lane where it does.
- **Whether `set_edited_scene_root()` is sufficient to make
  `update_configuration_warnings()` emit inside a `[SceneTree]` doctest** is read from
  `scene/main/node.cpp:3498-3508` and not run. Nothing in this repo currently connects to
  `node_configuration_warning_changed` — the T9/T9b/T10/T10b/T11/T12/T13 harness is the first
  user of it, so it is the least-precedented part of §6.4.
- **Whether an unloaded imported `GaussianSplatAsset` reports `get_splat_count() == 0` and
  later emits `changed`** — the trigger conjunct P (§6.1) depends on for lazily-loaded
  content — is not measured, and **T11/T12/T13 do not close this gap.** They drive the *setter*
  path, where the emission is read directly from source (`core/gaussian_splat_world.cpp:120`,
  `core/gaussian_splat_asset.cpp:895`), so they prove the node-side wiring reacts to a
  `changed` that fires. Whether the *import* path fires one is a different question about the
  importer, not about this warning, and it remains the residual recorded in §6.3.

## 9. Follow-up work this ADR surfaces but does not do

- **Hidden instance nodes steer the global route.** The instance-hint fallback loop at
  `core/gaussian_splat_scene_director.cpp:2830-2846` ignores `record.visible` while the same
  store's `visible` flag is honoured at `:1603`, `:1754`, `:1929`, `:2075` and `:2923`.
  Worth filing as its own issue: a hidden node influencing route selection is surprising
  independently of #788, and it is a small, testable change. **Not** to be folded into the
  Option 1 PR — it is a behaviour change to the director, not a diagnostic.
- **Instance records that outlive their payload still steer the global route.** Same loop,
  same defect shape as the bullet above, different field. When `refresh_asset()` fails it
  evicts the cached `GaussianData` (`core/gaussian_splat_scene_director.cpp:795`, and
  `retain_asset()` at `:760`, both via `_evict_asset_data()` at `:681-690`) and
  `register_instance()` returns without removing the `InstanceRecord` (`:1146-1148`). Every
  buffer builder then skips the record for null data,
  but the hint loop at `:2830-2846` still counts it, so the route is committed to `RESIDENT`
  for a record that contributes no instances — landing on the `resident_no_instances` skip of
  §3.1 with nothing on screen. The loop's filter should test what the render path tests, not
  a flag that survives its own payload. This is also the source of the one accepted false
  negative in §6.1's predicate; fixing it here would remove that caveat.
- **#862 — `GaussianSplatWorld3D` never resubmits when the assigned `GaussianSplatWorld`'s
  payload changes.** `SubmissionStore::store_submission()` copies the payload `Ref`s into the
  record (`core/gaussian_splat_scene_director.cpp:828-841`) and nothing re-publishes when the
  already-assigned resource is mutated, so the director keeps the apply-time snapshot. **This
  is a hard prerequisite of Option 1, not merely a dependency** — see the Status block and
  §5.1. §6.1's conjunct P reads the resource, so the two diverge in both directions: adding a
  payload gives a false positive, and **removing one gives a false negative** in which the
  warning goes silent while the stale submission keeps steering the route. Filed with its own
  two-directional mutation proof. **Not** to be folded into the Option 1 PR — it changes what
  the renderer draws, not what the editor says; Option 1 waits for it instead.
- **#863 — `GaussianSplatWorld3D` handles neither `NOTIFICATION_ENTER_WORLD` nor
  `NOTIFICATION_EXIT_WORLD`,** so a viewport `World3D` switch strands its submission in the old
  scenario while the node resolves the new one. **This is the reason §6.1's conjunct S compares
  registered scenarios rather than resolved ones**; that choice makes the predicate correct
  before *and* after the fix, so Option 1 does not block on it. **What it does cost, until it
  lands, is a step in the user-facing remedy:** separating the two `World3D`s — the action
  §6.2 recommends — leaves the world node's submission in the old scenario, so the warning
  survives the fix the user just performed. §6.2 now names the step that completes it
  (re-apply the world node), and round 6 verified that step against
  `core/gaussian_splat_scene_director.cpp:2509-2517`, which releases the stranded submission
  rather than duplicating it. That mitigation is why #863 is recorded as a strong dependency
  and not, like #862, as a blocker. Also **not** to be folded into the Option 1 PR: §6.3 adds
  those two notification cases to the class for a *refresh* only, and making them migrate the
  submission is a renderer behaviour change.
  **One further consequence, recorded here rather than filed separately because it is
  downstream of this same defect:** under a headless run the stranded node's
  `last_known_scenario` *does* move to the new scenario on the next process tick
  (`nodes/gaussian_splat_world_3d.cpp:223` → `:326-328`, armed at `:118-120`), so
  `NOTIFICATION_PREDELETE` then calls `try_prune_world_if_unused()` on the wrong scenario
  (`:184`) and the old `SharedWorld` is never pruned. It is worth attaching to #863's fix as a
  test, not a separate issue.
- **#869 — `GaussianSplatWorld3D` writes `last_known_scenario` before
  `submit_world_submission()` can reject it.** The cache is assigned at
  `nodes/gaussian_splat_world_3d.cpp:491-493` (and, on the `apply_world()` path, earlier still
  at `:326-328` via `_ensure_renderer()`), the submit runs at `:516`, and the arbitration
  rejection returns at `:521` without rolling either the cache or
  `was_world_submission_active` back. A previously-applied node re-applied into a scenario
  another live world owns therefore ends with **S naming B while its submission still steers
  A, and I still `true`** (§6.1). **Not** to be folded into the Option 1 PR — it is a write
  ordering fix in the node with a lifetime consequence of its own (PREDELETE prunes the wrong
  scenario at `:184`, so the `SharedWorld` for A is never reclaimed — the PR-4-of-#352 leak
  through a different door), and it needs the two-direction proof filed with it.

  **Why this is *not* a prerequisite, when #862 is — the test applied, not a blanket answer.**
  #862 blocks because its false negative is reachable **in the exact scene the warning
  targets**: one world node, one instance node, one `World3D`, one inspector write to an
  already-assigned resource, no other defect present. That is T1's setup, so the diagnostic
  goes silent in the configuration it exists for. #869's false negative is not reachable in
  that scene at all. The rejection at
  `core/gaussian_splat_scene_director.cpp:2392-2401` fires **only** when a second, live
  `GaussianSplatWorld3D` already owns the destination scenario, so the state needs two world
  nodes *plus* the instance node *plus* a `World3D` migration *plus* a re-apply — and in that
  scene the user's undiagnosed problem is already two world nodes in one scenario, which §6.1
  states this ADR does not diagnose. The warning going quiet there removes a diagnosis this
  ADR never promised for that configuration; #862's removes one it does.

  Two further checks, because "less reachable" alone is not the standard round 7 set. **It does
  not break the §6.2 remedy**, which is what kept #863 off the prerequisite list: §6.2 tells
  the user to put one node under a `SubViewport` with its **own** `World3D`, and a freshly
  created own-world scenario has no incumbent owner, so the re-apply is accepted and the
  migration path round 6 verified (`:2509-2517`) runs unchanged. And **it does not make the
  predicate unimplementable**: unlike #862, where no wording and no conjunct reaches the
  divergence, #869 is closed by the fix landing later without any change to §6.1 — S simply
  stops moving on a rejected submit. Option 1 can ship, and the residual is recorded in §6.1
  and §8 rather than hidden.
- **#855 must be root-caused before Option 2 is scheduled** (§5.2, reason 3).

#862, #863 and #869 were all found *through* this ADR rather than by it: each round of review on
the spec has turned up a product defect in the class the spec describes, and all three are the
same shape — **a node-side field written optimistically, before the director operation that
decides whether the field is true.** That is worth recording as a property of the exercise — a diagnostic that has to state, conjunct by conjunct, when a node
"would steer the route" is an audit of the registration path whether or not it was meant to be.

## 10. What would change this decision

- **#862 lands** → Option 1 becomes implementable and §6 can be built as written. Nothing
  else about the decision changes; this is the gate, not a revision.
- **#862 is declined or deferred indefinitely** → the decision has to be reopened, because the
  only remaining ways to ship the warning are to accept a diagnostic that goes silent while
  the failure is live, or to base conjunct P on a live director query, which §6.1 rejects on
  its own evidence. Neither is a decision this ADR has made; it would need a new one.
- **Option 2 lands** → the warning and this ADR's §6 are deleted; the ADR stays as the
  record of why the interim existed.
- **Evidence that users ship the broken combination despite the warning** → revisit Option 3
  as an *export-time* check (§5.3), never as a runtime error.
- **A measurement establishing why the world submission failed the hint gate** (§3.2, §8) →
  if it shows the outcome is deterministic for an authored scene, the §6.2 warning must be
  tightened to name *which* node type renders nothing, because a warning vaguer than the
  measured truth is its own defect. Until such a run exists the warning states the outcome
  and no cause — the reverse error, shipping an unmeasured causal claim to users, is the one
  this ADR already made once and corrected.
