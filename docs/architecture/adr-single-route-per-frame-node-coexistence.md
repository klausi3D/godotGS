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
- **Tracking:** #788 (this decision). **Split from:** #785 (`qa_visual_diff` /
  `qa_sh_rotation` were built on the combination this ADR describes; fixed by PR #854).
  **Blocked-on / related:** #855 (world route drops content the instance route draws).
  **Waiting on Option 2:** `qa_stream_multi_asset.tscn`.
- **Verified against:** `origin/master` @ **`a04472a82cf`**. Every file:line in this
  document was read at that commit. §2 records which of the issue's original citations had
  drifted.

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
                  AND N.get_world_3d().is_valid()                           (R3)
                  AND payload(N)                                            (P)
                  AND submission_intent_live(N)                             (I)

  warn on N       ≡   WOULD_STEER(N)
                  AND ∃ M in the *other* group with
                          WOULD_STEER(M)
                      AND M.get_world_3d()->get_scenario()
                            == N.get_world_3d()->get_scenario()
  ```

  Every conjunct is a mirror of a guard on the path that actually produces the submission, and
  is listed here with that guard. Nothing is in the predicate that is not:

  | Conjunct | Mirrors, on the world node | Mirrors, on the instance node |
  | --- | --- | --- |
  | R1 | `_register_shared_renderer()` returns early when `!is_inside_tree()` (`nodes/gaussian_splat_world_3d.cpp:475-477`) | `_register_instance_in_director()` returns early when `!in_tree` (`nodes/gaussian_splat_node_3d.cpp:2590-2592`) |
  | R2/R3 | the scenario it publishes is `get_world_3d()->get_scenario()` (`:488`) | same guard, `!in_world` (`:2590-2592`) |
  | P | `SubmissionStore::record_has_renderable_payload()` (below) | the asset resolution in `_register_instance_in_director()` (below) |
  | I | `was_world_submission_active` (below) | **constant true** — the class has no apply gate (below) |

  `is_inside_world()` is **not** redundant with `is_inside_tree()`. `Node3D::get_world_3d()`
  trips `ERR_FAIL_COND_V(!is_inside_world(), ...)` — returning null *and printing an engine
  error* — for a node that is in the tree but between worlds
  (`scene/3d/node_3d.cpp:1054-1060`), and a `SubViewport` world switch produces exactly that
  state for the duration of one propagation (§6.3, fact 3). The guard is required on each
  candidate peer as well as on self, because the condition dereferences the peer's
  `get_world_3d()` too.

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
  another world already owns the scenario (`nodes/gaussian_splat_world_3d.cpp:516-522`), so a
  world node that lost world-vs-world arbitration reads I = false and stays silent. **Recorded,
  not hidden:** that silence is correct for *this* warning (a rejected submission does not
  steer the route), but the user's actual problem in that scene is two world nodes in one
  scenario, which this ADR does not diagnose. §8 records the measurement that has to confirm I
  latches at all in the editor, and the fallback if it does not.

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
A GaussianSplatNode3D shares this node's World3D. The renderer commits to a single
render route per frame, so only one of the two node types is drawn and the other
renders nothing. Hiding either node does not restore the other. Until per-submission
routing lands (issue #788), keep GaussianSplatWorld3D content and GaussianSplatNode3D
content in separate World3Ds: put one under a SubViewport with its own World3D, or run
the two as separate scenes one at a time. Splitting them into separate .tscn files that
are then instantiated under the same viewport does NOT help — they still resolve the
same World3D.
```

On `GaussianSplatNode3D`:

```text
A GaussianSplatWorld3D shares this node's World3D. The renderer commits to a single
render route per frame, so only one of the two node types is drawn and the other
renders nothing. Hiding either node does not restore the other. Until per-submission
routing lands (issue #788), keep GaussianSplatNode3D content and GaussianSplatWorld3D
content in separate World3Ds: put one under a SubViewport with its own World3D, or run
the two as separate scenes one at a time. Splitting them into separate .tscn files that
are then instantiated under the same viewport does NOT help — they still resolve the
same World3D.
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
draft of this section got the world switch wrong.

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
- The condition guards `is_inside_world()` on self and on every peer before touching
  `get_world_3d()` (§6.1), for the same reason: a node can be in the tree and out of a world
  for the duration of a viewport switch.

Call the helper, plus a deferred `update_configuration_warnings()` on self, from — and, per
"Triggers deliberately omitted" below, **only** from:

| Trigger | Conjunct it can flip | `GaussianSplatWorld3D` | `GaussianSplatNode3D` |
| --- | --- | --- | --- |
| `NOTIFICATION_ENTER_WORLD` / `NOTIFICATION_EXIT_WORLD` | R1/R2/R3 | **add both cases** (the class handles neither today — it has only `VISIBILITY_CHANGED` at `:207`) | add (cases exist at `:445`, `:449`) |
| content assigned / applied / cleared | P and I | `set_world()` (`:249`, which applies when in-tree at `:252-254`), `apply_world()` (`:301`), `clear_world()` (`:306`) — the three sites that move `was_world_submission_active`. This class calls `update_configuration_warnings()` nowhere today, so both the self and the peer call are new | P only — the sites that already call `update_configuration_warnings()` (`:738`, `:827`, `:1063`, `:1283`, `:1293`, `:1303`, `:2969`); add the *peer* notification there |
| content **count** changes on an already-assigned resource | P | **new:** connect to the `GaussianSplatWorld` resource's `changed` signal and refresh self + peers, mirroring what the instance node already does for its asset. `GaussianSplatWorld::set_gaussian_data()` (`core/gaussian_splat_world.cpp:107`) emits it at `:120`, as do `set_chunk_payload_source()` (`:158`) and `set_payload_metadata()` (`:214`) | already covered on self: `set_splat_asset()` connects the asset's `changed` (`:730-733`) and `_on_asset_changed()` calls `update_configuration_warnings()` (`:2969`) — add the *peer* notification there. `GaussianSplatAsset::set_positions()` (`core/gaussian_splat_asset.cpp:876`) sets `splat_count` at `:885` and emits `changed` at `:895`; `set_splat_count()` (`:308`) emits at `:326` |

The last row exists **because** the predicate reads a count (P, §6.1) rather than a
non-null `Ref`. A resource that is assigned while empty and populated later — an import that
completes, a `set_positions()` call, a streamed world payload — flips the answer with no
assignment happening on the node, so assignment-only triggers would leave the warning
permanently wrong on exactly the scenes most likely to hit it. **Residual, accepted:** a
`GaussianSplatWorld` payload that grows without emitting `changed` leaves the warning stale
until the next world transition or reassignment. That is a quieter error than a warning
that blinks, and it is listed in §8 as unverified-by-run. T11 and T12 (§6.4) are the cases
that make this row's wiring falsifiable rather than merely specified.

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
  (`:410-412`), i.e. unless I is already true, and it never sets it false.

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
| **T1** | **the RED-without / GREEN-with case** | `GaussianSplatWorld3D` with a world holding ≥1 splat **and** `GaussianSplatNode3D` with runtime splat data, both under one `SceneTree` root, same `World3D` | **both** nodes report the warning. `if (!found) { FAIL(...); return; }` | reverting either `get_configuration_warnings()` addition |
| **T2** | control — world alone | only the `GaussianSplatWorld3D` | warning **absent** | making the condition "a GS node of *either* type is in this scenario" |
| **T3** | control — instance alone | only the `GaussianSplatNode3D` | warning **absent** | same as T2 |
| **T4** | control — two instance nodes | two `GaussianSplatNode3D`s, no world node | warning **absent**. This is the control that stops "more than one GS node in the scene" from passing as an implementation; multiple instance nodes coexist correctly, they all publish `RESIDENT` | replacing the *other*-group lookup with a both-groups lookup |
| **T5** | control — visibility is irrelevant | T1 setup, then `set_visible(false)` on the instance node, then on the world node | warning **still present in both configurations** (pins §6.3) | adding `&& is_visible_in_tree()` to the condition |
| **T6** | control — `route_policy` is irrelevant | T1 setup, run once with `route_policy = 0` and once with `= 1`, restoring the setting via the existing `ProjectSettingGuard` pattern (`tests/test_node_surface_cleanup.h:184`) | warning **present at both values** (pins §3.4/§6.3) | making the condition read `gs::settings::get_streaming_route_policy()` |
| **T7** | control — different `World3D` | world node in the main tree, instance node inside a `SubViewport` with `own_world_3d = true` | warning **absent in both**. This is the control that proves the scenario scoping is real rather than a global "is there a world node anywhere" check | dropping the scenario-RID comparison from the condition |
| **T8** | control — no content at all | both node types present, neither has any resource assigned | warning **absent** — the user's actionable problem is "no asset", which the existing warning already states | making the condition ignore content entirely |
| **T8b** | control — **empty instance resource** | world node with ≥1 splat **and** instance node whose `splat_asset` is a valid `GaussianSplatAsset` with `get_splat_count() == 0` | warning **absent on both**, and the world node gains no other new warning. The world route renders in this configuration (§6.1 proof), so a warning here would be a lie | weakening the instance predicate to `splat_asset.is_valid()` — i.e. the "content assigned" spec this ADR shipped in round 1 |
| **T8c** | control — **empty world resource** | instance node with ≥1 splat of runtime data **and** world node whose `GaussianSplatWorld` has a null-or-zero-count `gaussian_data` *and* `chunk_payload_source` | warning **absent on both** | weakening the world payload test (P) to `get_world().is_valid()` |
| **T8d** | control — **world never applied** | T1 setup, except the world node has `auto_apply_on_ready = false` set *before* it enters the tree. Payload is fully renderable; `apply_world()` is never called (`nodes/gaussian_splat_world_3d.cpp:108-113`) | warning **absent on both** — the instance route renders in this configuration | dropping conjunct I from the world predicate |
| **T8e** | control — **world explicitly cleared** | T1 setup (so the world *did* apply and the warning is present — assert that first), then `world_node->clear_world()`, then flush | warning **absent on both** afterwards, even though `get_world()` is still valid and still non-empty (`clear_world()` never nulls the `Ref`, `:306-317`) | dropping conjunct I; **and, uniquely,** implementing I as `is_auto_apply_on_ready()` — the natural misreading of "apply intent", which reads `true` here and which T8d cannot catch |
| **T9** | the peer refresh on **tree exit** actually happens | T1 setup + the signal harness below, then `root->remove_child(instance_node)`, then flush | the world node appears in the recorder **and** *every* snapshot recorded for it no longer carries the conflict warning | (a) deleting the `EXIT_WORLD` peer refresh — now a real kill, because §6.3 no longer specifies an `EXIT_TREE` refresh to survive it; (b) issuing any refresh immediately instead of deferred |
| **T9b** | the peer refresh on **tree entry** actually happens | root already holding the T1 world node (applied, warning absent) + the signal harness, then `root->add_child(instance_node)` carrying ≥1 splat, then flush | the world node appears in the recorder **and** *every* snapshot recorded for it now **contains** the conflict warning | deleting the `ENTER_WORLD` peer refresh. **No immediacy mutation:** fact 1 makes an immediate enter-time refresh correct, so T9b cannot kill one and must not claim to |
| **T10** | the peer refresh survives a **`World3D` switch** | T1 setup with the instance node under a `SubViewport` that shares the main world (the default), + the signal harness, then `subviewport->set_use_own_world_3d(true)`, then flush | same as T9, for the world node left behind in the old world | (a) **scoping the peer walk by scenario** — the only mutation T10 uniquely kills; (b) issuing any refresh immediately instead of deferred. **Per-trigger deletion is not claimed** — see the derivation below |
| **T11** | the **world** resource-`changed` wiring is real | T8c setup (empty world resource, no warning) + the signal harness. Assert absence, clear the recorder, then `world_res->set_gaussian_data(<≥1 splat>)` (`core/gaussian_splat_world.cpp:107`, emits at `:120`), then flush | the recorder holds an entry for the **world node** *and* one for the **instance node**, and both snapshots now **contain** the conflict warning | (a) deleting the world node's `changed` connection — no entry at all; (b) keeping the connection but refreshing only self — the instance-node entry disappears while the world-node entry survives, which is why the peer half is asserted separately |
| **T12** | the **instance** resource-`changed` wiring is real | T8b setup (empty `splat_asset`, no warning) + the signal harness. Assert absence, clear the recorder, then `asset->set_positions(<3 floats>)` (`core/gaussian_splat_asset.cpp:876`, sets `splat_count` at `:885`, emits at `:895`), then flush | same as T11, with the roles swapped | omitting the *peer* notification added to `_on_asset_changed()` (`nodes/gaussian_splat_node_3d.cpp:2965-2970`) — the world-node entry disappears. The self entry is pre-existing wiring (`:2969`); T12 additionally regression-pins it, but must not claim it as the new-wiring kill |

**T9, T9b, T10, T11 and T12 need a signal observer, not a getter.**
`get_configuration_warnings()` recomputes from the groups and from the live resources on every
call, so once the trigger has *fully completed* it returns the right answer whether or not any
refresh was ever issued. A case that only calls the getter therefore stays green with the
entire peer-notification mechanism deleted — a green test for a mechanism that is not there,
and the exact defect shape this ADR is trying to keep out of the module. **The same trap
applies to the resource-`changed` wiring**, which is why T11/T12 use this harness rather than
reading warnings after populating a resource: the predicate recomputes the count on demand, so
deleting the `changed` connection entirely leaves a getter-only case green. All five cases
must observe what the editor observes:

1. `SceneTree::set_edited_scene_root(root)` (`scene/main/scene_tree.cpp:1623-1627`,
   `TOOLS_ENABLED`-only; the module test batches build under `target=editor tests=yes`).
   Without it `Node::update_configuration_warnings()` returns before emitting
   (`scene/main/node.cpp:3501-3506`) and no signal is observable at all. Save and restore the
   previous value.
2. Connect a recorder to the tree's `node_configuration_warning_changed` signal
   (`scene/main/scene_tree.cpp:1952`). For each emitting node the recorder stores **the
   result of calling `get_configuration_warnings()` on that node at signal time** — not just
   the fact that it fired.
3. Assert the case's **starting** state first — for T9/T10 the T1 precondition (the conflict
   warning present on both nodes), for T9b/T11/T12 its absence — then clear the recorder, then
   perform the trigger, then `MessageQueue::get_singleton()->flush()` so the deferred refreshes
   of §6.3 run.
4. Assert: the recorder holds an entry for the peer node, **and *every* snapshot it recorded
   for that node** carries the expected end state (T9/T10: no conflict warning; T9b/T11/T12:
   the conflict warning present). T11/T12 assert this for **both** nodes — self and peer —
   because the two halves of the wiring fail separately.

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

**What T9/T10 do *not* prove, stated rather than papered over.** Per-trigger *deletion* is
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
- What is left for T10, and what no other case covers, is the **scenario-scoping** mutation:
  scope the peer walk by scenario and the `EXIT_WORLD` walk resolves no scenario at all
  (fact 3a) while the `ENTER_WORLD` walk resolves the *new* one and excludes the old-world
  peer — no entry, RED. That is T10's reason to exist.

This is the second time this section has had to be re-derived. Round 1 mandated "T1 fails,
T2–T9 pass", which no mutation could produce. Round 2 replaced it with per-trigger Run B
expectations that were equally unproducible, because `Node3D` dispatches `EXIT_WORLD` from its
own `EXIT_TREE` handler (`scene/3d/node_3d.cpp:201`) and the two refreshes masked each other.
**The rule this leaves behind: before writing a "Killed by" entry, enumerate which
notifications the trigger actually dispatches and which refresh paths survive the mutation. If
two paths reach the same assertion, either pick a trigger that reaches one, or drop the
attribution. Do not write the transcript you expect to see.**

**Spurious-fire control for all five:** with the recorder connected and the same T1 setup,
remove an unrelated plain `Node3D` from the root and flush. The recorder must contain **no**
entry for either GS node. Without this, "the peer was signalled" could be satisfied by any
unrelated tree churn that happens to emit for the same node.

#### The mutation runs — what they must actually produce

**The mutation runs are part of the deliverable, not optional.** Two runs are mandated, and
the expected outcomes below were derived from the case list above rather than assumed.

**Run A — the broad mutation.** Revert both `get_configuration_warnings()` additions and
keep everything else (groups, helper, triggers, deferral). No conflict warning is producible
at all, so every case that *positively requires* the warning fails and every case that
requires its *absence* passes:

> expected: **T1, T5, T6, T8e, T9, T9b, T10, T11, T12 FAIL —
> T2, T3, T4, T7, T8, T8b, T8c, T8d PASS.**

Derived case by case, not assumed. T5 and T6 fail because each positively asserts the same
warning T1 does, under a perturbation. T8e fails on its *first* assertion — it must observe
the warning present before `clear_world()` in order for its absence afterwards to mean
anything — while T8d, which asserts absence throughout, passes. T9 and T10 fail on their T1
precondition (step 3 above), which they must assert, because a T9 that skipped it would be
green against a mutant that produces no warning at all, i.e. vacuous. T9b, T11 and T12 pass
their absence precondition and then fail on the positive post-condition: the signals still
fire (helper and triggers are intact in Run A) but no snapshot ever contains a warning that no
longer exists.

**A transcript reading "T1 fails and everything else passes" is not achievable for this
mutation and must not be written.** If a run does produce it, the assertions are matching
something other than the new warning — most likely the pre-existing "No Gaussian splat asset…"
string — and the whole suite proves nothing; the three-substring helper above exists to
prevent exactly that.

**Run B — the narrow mutations, one per distinct mutation.** Run A proves the warning exists;
it proves nothing about *which* case guards *what*, because a mutation that kills nine cases at
once cannot attribute any of them. Apply each **distinct** mutation named in the "Killed by"
column independently and record for each that **the named case is RED while T1 stays GREEN**.
The distinct mutations, and which case is expected RED for each:

| Mutation | Expected RED |
| --- | --- |
| condition = "a GS node of either type is in this scenario" | T2 **and** T3 (they share it) |
| other-group lookup → both-groups lookup | T4 |
| add `&& is_visible_in_tree()` to the condition | T5 |
| condition reads `get_streaming_route_policy()` | T6 |
| drop the scenario-RID comparison from the condition | T7 |
| condition ignores content entirely | T8 |
| weaken P on the instance side to `splat_asset.is_valid()` | T8b |
| weaken P on the world side to `get_world().is_valid()` | T8c |
| drop conjunct I | T8d **and** T8e (they share it) |
| implement I as `is_auto_apply_on_ready()` | T8e only — T8d stays GREEN, which is the point of having both |
| delete the `EXIT_WORLD` peer refresh | T9 |
| delete the `ENTER_WORLD` peer refresh | T9b |
| issue refreshes immediately instead of deferred | T9 **and** T10 |
| scope the peer walk by scenario | T10 |
| delete the world resource `changed` connection | T11 |
| refresh only self from the world `changed` handler | T11 |
| omit the peer notification in `_on_asset_changed()` | T12 |

Two entries in that table are **deliberately absent** and their absence is the round-3
correction: there is no "delete the `EXIT_TREE` peer refresh" row (§6.3 no longer specifies
one) and no row attributing a deletion to `EXIT_WORLD` *versus* `ENTER_WORLD` within T10 (the
derivation above shows no case can). Each is a one-line
result and none requires rebuilding more than the two node translation units.

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
- **Cost:** an editor-only code path in two node classes, one public const accessor on
  `GaussianSplatWorld3D` (§6.1, conjunct I), plus seventeen test cases (T1–T12 with
  T8b/T8c/T8d/T8e and T9b) and the spurious-fire control. The peer notification is the only
  non-trivial part, and it runs on world transitions, resource `changed` and content
  assignment — never per frame. Every refresh is deferred by one idle frame (§6.3), which is
  invisible in the editor and is why T9/T9b/T10/T11/T12 flush the message queue.

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
- **Whether `set_edited_scene_root()` is sufficient to make
  `update_configuration_warnings()` emit inside a `[SceneTree]` doctest** is read from
  `scene/main/node.cpp:3498-3508` and not run. Nothing in this repo currently connects to
  `node_configuration_warning_changed` — the T9/T9b/T10/T11/T12 harness is the first user of
  it, so it is the least-precedented part of §6.4.
- **Whether an unloaded imported `GaussianSplatAsset` reports `get_splat_count() == 0` and
  later emits `changed`** — the trigger conjunct P (§6.1) depends on for lazily-loaded
  content — is not measured, and **T11/T12 do not close this gap.** They drive the *setter*
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
- **#855 must be root-caused before Option 2 is scheduled** (§5.2, reason 3).

## 10. What would change this decision

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
