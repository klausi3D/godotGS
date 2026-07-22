# ADR: Decompose the `GaussianSplatNode3D` god-class by ownership, not friendship

- **Status:** Proposed — awaiting owner sign-off. Design-note-before-implementation;
  no production code changes in this PR. **Revised twice under review**; round-2 corrections
  (false "pure `QualityPolicy`" premise, missing P4, field count 87, retracted vacuity claim,
  risk labels) are marked inline.
- **Risk class:** **R0 for this document.** It is docs-only (`docs/architecture/**`), and
  per `docs/governance/agentic-engineering.md:83` R0 is *"Docs and agentic governance only."*
  (Corrected in review round 2: the previous revision labelled this ADR **R2**, which per
  `:85` is the *renderer/GPU* class and would demand runtime/GPU evidence this document
  cannot produce.) The *implementation* it authorizes spans R1→R3; each step carries the
  risk class of the files it edits, re-derived by CI from the diff. See the per-step labels
  in §"Staged migration" — note in particular that `:86` makes **public API/compat** an
  **R3** trigger, so *every* step that touches the 38 `ADD_PROPERTY` registrations, the 100
  `ClassDB::bind_method` bindings, or the `_set`/`_get` compat shim
  (`gaussian_splat_node_3d.cpp:576ff`) is **R3** — not only Step 3.
- **Base SHA:** `237a4b1cc3965fdbd6f12dec825c0e2077b2e9ce` (`origin/master` at authoring time).
- **⚠ Anchor status (review round 3) — the round-2 "byte-identical" claim is now FALSE and
  the §B1 matrix needs re-anchoring before Step 2 is authored.** Round 2 stated that
  `gaussian_splat_node_3d.{h,cpp}` and `gaussian_splat_node_helpers.cpp` were byte-identical
  between the base and master, so every anchor held against both. That was true at
  `9161d92f349`. It is **not** true at `b15c6ddda46`: two further commits have landed on
  `modules/gaussian_splatting/nodes/` — `ab847aeabf3` (*"converge P2 shared-renderer gating
  on peer-set change + gate painterly (#329) (#667)"*) and `d6def0641aa` (*"delete two dead
  scene-effector helpers … (#680)"*) — for a combined `node_3d.cpp +96`, `node_3d.h +11`,
  `helpers.cpp +30`.

  Material consequences, because they land **on the frozen §B1 matrix itself**:
  - **The two painterly rows below are superseded by #667.** At the base, painterly renderer
    writes were P1-only. At master they are **P1 *and* P2**, with `set_painterly_enabled`
    *forced to `false`* when the renderer is shared: `helpers.cpp:1415-1421` —
    `const bool painterly_shared = _is_renderer_shared_with_other_content(owner);
    owner.renderer->set_painterly_enabled(painterly_shared ? false : owner.enable_painterly);
    if (!painterly_shared) { …edge_threshold/stroke_opacity/stroke_length/gamma… }`. Both
    rows are annotated inline below.
  - The colour-grading anchor moved `helpers.cpp:1403-1410` → `:1423-1429`.
  - `node_3d.h` gained `_converge_shared_renderer_state()` /
    `_notify_renderer_peers_shared_state_changed()` — a **new P2 surface** that the concern
    map does not list.

  Anchors that **did** survive unchanged and remain valid against both SHAs: `node_3d.cpp:517-536`,
  `:41-58`, `:1011-1089`, `helpers.cpp:1294-1325`, `:1368`, `:697/710/729`, `:699/712/731`,
  `:781`, `:995`, `node_3d.h:123-128`.

  **This is a re-freeze, not a re-anchor.** #667 deliberately *changed* P2 behavior, so §B1
  cannot simply be re-pointed at new line numbers — the matrix must be re-derived against a
  current base and D5 re-confirmed against the new behavior. Until that happens, a Step 2
  matrix test written from the rows below would encode pre-#667 semantics and fail on head.
  Only invariant A's `gaussian_splat_world_3d.cpp:112-166` anchor was previously flagged for
  re-checking; that still stands, and is now the smaller of the two problems.
- **Primary issue:** #552 (decompose the god-class). **Also closes / advances:** #551
  (PREDELETE ordering → structural), #550 (per-frame camera writers → one writer).
  **Coordinates with:** #578 / #516 / #517 (world-node lifecycle, in flight), #558
  (script-instantiable internals), #357 & #547 (settings ownership / config-loader dedup),
  #299 (missing node integration coverage), #592 (editor preview worker — adjacent).

> **Issue-number note for reviewers.** The task brief referred to the god-class as #548,
> the camera writes as #547, and "settings-ownership #544". The *actual* GitHub issues are
> **#552** (god-class), **#550** (camera writes), and **#357/#547** (settings). #548 is a
> `GaussianData` naming collision and #544 is the uint32 streaming-buffer cap — unrelated.
> This ADR uses the verified numbers.

## Context / problem

`nodes/gaussian_splat_node_3d.{h,cpp}` is 870 + 2,759 lines and delegates to a 1,499-line
`gaussian_splat_node_helpers.cpp` holding **six friend-helper objects**. Issue #552's fix
direction is precise: *"carve real ownership seams … as owned members, not friends. …
friend-decomposition widens state aliasing instead of shrinking it."*

That is exactly what exists today. The helpers are declared `friend` and each holds a bare
`GaussianSplatNode3D &owner` (`gaussian_splat_node_helpers.h:11-111`), and the node header
itself documents that they *"access 50+ private fields and methods"*
(`gaussian_splat_node_3d.h:118-128`). The six helpers are instantiated as value members
(`gaussian_splat_node_3d.h:258-263`) but every method reaches back into `owner.<private>`.
The result is one shared mutable-state blob split across two files — the surface area is not
reduced, only relocated. All **87** private fields (`gaussian_splat_node_3d.h:131-263`)
remain co-owned by seven objects.

> **Field count corrected to 87 (review round 2).** Earlier revisions said "~50" (inherited
> from the header's own stale `50+` comment) and an intermediate review said **77**. Both are
> undercounts. The verified figure is **87** member-variable declarations in the private
> block `h:131-263`, counted by stripping trailing `//` comments and treating a `(` that
> appears *after* the `=` as an initializer rather than a parameter list. The two ways to get
> 77 are both methodology artifacts, reproduced exactly: truncating the region at `h:257` drops
> the **6** helper composites (`h:258-263`), and a comment-blind line filter drops the **4**
> declarations carrying trailing comments (`h:142`, `:143`, `:154`, `:163`); 87 − 6 − 4 = 77.
> A naive `(`-excluding filter additionally drops the 3 value-initialized fields `h:173`
> (`Vector3 wind_direction = Vector3();`), `h:225` (`ObjectID cached_viewport_id = ObjectID();`)
> and `h:231` (`Vector2i cached_viewport_size = Vector2i();`) while falsely counting the
> `};` at `h:223` that closes the `ViewportTextureState` enum. **Use 87.** The header's own
> `50+` comment at `h:120` is stale by 37 fields and should be corrected by whichever step
> first edits that block.

### Current responsibility map — every concern the node owns

| # | Concern | State (h) | Entry points | Helper (friend) |
|---|---------|-----------|--------------|-----------------|
| 1 | **Asset binding** — imported PLY/SPZ resource, connect/disconnect `changed`, reload with `CACHE_MODE_REPLACE` 3-failure handling, import-metadata flags, bounds recompute, `ply_file_path` legacy migration | `splat_asset`, `runtime_asset`, `asset_loading`, `asset_lod_enabled`, `asset_optimize_for_gpu` (`:131-143`) | `set_splat_asset` (`cpp:686`), `_load_asset` (`cpp:1792`), `_update_asset`, `_clear_asset`, `_on_asset_changed`, `_set` migration (`cpp:594`) | AssetHelper (`helpers.cpp:136-272`) |
| 2 | **Procedural / manual data** — validate 11 arrays, build `GaussianData`, synthesize opacity/SH from colors, compute bounds, build runtime asset | `renderer_data` (`:237`) | `set_splat_data` + 8 `_…splat…` privates (`cpp:723-971`) | — (on node) |
| 3 | **Quality / LOD / streaming policy** — 4 preset tables ×~28 keys, tier caps, clamps, effective-config snapshot + WARN-on-cap logging | `quality_preset`, `lod_bias`, `max_render_distance`, `max_splat_count`, `lod_config`, `streaming_config`, `effective_config_snapshot` (`:136-147`) | `set_quality_preset`/`set_lod_bias`/… | QualityHelper (`helpers.cpp:760-1207`, ~450 lines — the largest concern) |
| 4 | **Painterly** — brush params + `PainterlyManager` | `enable_painterly`, `edge_threshold`, `stroke_opacity`, `stroke_width`, `color_variation`, `temporal_blend`, `painterly_seed`, `painterly_manager` (`:150-157`) | `set_enable_painterly`/…, `_apply_painterly_settings` (`cpp:2077`) | — (on node) |
| 5 | **Renderer registration + settings ownership** — shared `Ref<GaussianSplatRenderer>`, director instance register/params/transform, the **global settings-owner lease** | `renderer`, `render_state_dirty`, `shared_renderer_multi_instance_state` (`:236-239`) | `_ensure_renderer`, `_apply_renderer_settings`, `_register_shared_renderer`, `_register_instance_in_director` (`cpp:2405`), `_update_instance_params_in_director` (`cpp:2499`) | RendererHelper (`helpers.cpp:1294-1499`) + free-function owner map (`helpers.cpp:35-80`) |
| 6 | **GPU base storage** — `gaussian_base` + `render_instance` RIDs against `GaussianSplatStorage` singleton | `render_instance`, `gaussian_base`, `last_known_scenario` (`:207-214`) | `_ensure_gaussian_base`/`_release_gaussian_base`/`_sync_gaussian_storage` (`cpp:2197-2252`), `_update_render_instance` | — (on node) |
| 7 | **Viewport bootstrap / render-target tracking** — 3-state machine, observers, deferred bootstrap + first-frame render | `cached_viewport_*`, `observed_viewport`, `viewport_texture_state`, `*_deferred` (`:219-234`) | `_update_cached_render_target`, `_deferred_viewport_bootstrap`, `_on_viewport_*` | ViewportHelper (`helpers.cpp:274-597`, ~320 lines) |
| 8 | **Per-frame update + camera publication** — update-mode gate, dirty upload, **`renderer->set_camera_transform/projection` every frame** | `render_state_dirty`, `shared_renderer_multi_instance_state` | `process_gaussian_render` (`cpp:1633`), `update_splats` + 8 privates, `_update_viewport_render_state` (**camera writes `cpp:1590-1591`**) | — (on node) |
| 9 | **Visibility** — CPU distance/frustum cull, parent-visibility tracking | `visible_in_viewport`, `parent_visible`, `parent_visibility_target`, `update_mode` (`:160,215-217`) | `_update_visibility` (`cpp:1974`) | VisibilityHelper (`helpers.cpp:1209-1292`) |
| 10 | **Color grading** — two-track replay state machine, per-instance director routing, bake/restore | `color_grading`, `grading_pushed_for_current_data`, `grading_explicit_pending` (`:177-192`) | `set_color_grading` (`cpp:2631`), `_push_color_grading_to_renderer`, `_replay_color_grading_if_pending` (`cpp:1914`) | — (on node) |
| 11 | **Debug overlays / HUD** — 13 flags, settings-manager persistence, child `CanvasLayer`+`Control` | `show_*`, `debug_draw_mode`, `debug_overlay_opacity`, `debug_hud_*` (`:242-256`) | `set_show_*`, `_update_debug_hud_visibility` (`cpp:2141`) | DebugHelper (`helpers.cpp:599-758`) |
| 12 | **Scene effectors / wind** — layer mask, scope-root resolution, ancestor collection, per-instance wind override | `scene_effector_*`, `wind_*`, `effect_*_scale` (`:165-173`) | `_get_instance_wind_*` (`cpp:2361`), `_resolve_scene_effector_scope_root` | — (on node) |
| 13 | **Compat properties / serialization** — `_set`/`_get`/`_get_property_list`/`_validate_property`, legacy fields, `stats/*` virtuals, inspector hiding | — | `cpp:517-684` | — (on node) |
| 14 | **Stats / monitoring** | `visible_splat_count`, `total_splat_count`, `last_update_time_ms`, `gpu_memory_mb` (`:195-199`) | `get_statistics` (`cpp:1361`), `get_effective_config_snapshot` | — (on node) |
| 15 | **Lifecycle / notifications** — ENTER/EXIT tree+world, PREDELETE prune dance, manager register | — | `_notification` (`cpp:432-515`) | — (on node) |
| 16 | **Editor drag-drop** | — | `_can_drop_data_fw`/`_drop_data_fw` (`cpp:2715`) | — (on node) |

The six friends cover concerns 1, 3, 5, 7, 9, 11; ten more concerns live directly on the
node. No concern owns its state — every field is reachable by all seven objects.

### Load-bearing comment invariants that must become structural

The audit (#551) calls out that correctness rests on prose. Four invariants are encoded only
in comments and are silently breakable by a reorder:

- **A — PREDELETE unref-then-prune (#551).** Two ~40-line mirrored comment blocks
  (`gaussian_splat_node_3d.cpp:460-500`, `gaussian_splat_world_3d.cpp:112-166`) explain that
  `NOTIFICATION_EXIT_TREE` already ran `unregister → _prune_world_if_unused` but observed
  `refcount>1` (the node still holds its `renderer` `Ref`), so PREDELETE must (1) `renderer.unref()`
  **first** (`node:494`, `world:160`), then (2) explicitly call `try_prune_world_if_unused`
  (`node:496-498`, `world:162-164`). The intervening `_unregister_shared_renderer()` is a
  no-op for pruning. Any reorder reintroduces the F6-reload renderer leak that motivated the
  fix (Codex review comments #3294797692 / #3294797697 on PR #387; director contract at
  `gaussian_splat_scene_director.h:266-276`). **Correct today, prose-guarded.**

- **B — settings gating. NOT one invariant — three distinct predicates, plus a fourth
  precondition (P4) on the grading path.** (Corrected in review round 1; the earlier revision
  described this as a single "settings single-owner" rule, which would have licensed a
  decomposition that silently changed behavior. P4 and the director-registration ordering
  guard were added in review round 2, when the claim that grading is "ungated" was refuted.
  §B1 below is now the normative statement.)

  A file-static, mutex-guarded map (`g_renderer_settings_owner_mutex` /
  `g_renderer_settings_owner_lookup`, `helpers.cpp:35-36`, helpers at `:38-80`) elects one
  node as the settings owner per renderer. But **ownership is only one of three predicates
  in force**, and they are not equivalent:

  | | Predicate | Definition | Semantics when it denies |
  |---|---|---|---|
  | **P1** | `can_apply_renderer_settings()` — `helpers.cpp:1294-1325` | node holds the ownership lease for this renderer **and** is in tree/world **and** has local source data **and** the renderer's active scene data belongs to this node | write **dropped silently** |
  | **P2** | `_is_renderer_shared_with_other_content()` — `helpers.cpp:97-111` *and a duplicate* `cpp:41-58` | director reports `get_instance_count_for_renderer() > 1` **or** `has_world_submission_for_renderer()` | write **dropped, or the value forced to a safe default, for every node incl. the owner** |
  | **P3** | splat-count mismatch heuristic — `helpers.cpp:1385` | `renderer_splat_count > 0 && renderer_splat_count != local_splat_count` | write dropped + `WARN_PRINT_ONCE` (`:1387`) |
  | **P4** | `_can_push_color_grading_to_renderer()` — `gaussian_splat_node_3d.cpp:1880-1882` | `renderer.is_valid() && is_inside_tree() && is_inside_world() && _has_local_source_data()` — **P1's precondition prefix without the lease** | grading push returns `false`; replay flags left armed |

  **P4 is not an independent fourth rule (added and scoped in review round 2).** Compare it
  against P1's opening at `gaussian_splat_node_helpers.cpp:1295-1304`: the two are the *same*
  three checks in the same order (`renderer.is_valid()`, `is_inside_tree() &&
  is_inside_world()`, `_has_local_source_data()`). P1 then adds two things P4 omits — the
  active-scene-data-ownership test (`:1312-1322`) and the lease claim/release
  (`:1319`/`:1324`). **Record P4 as "P1's precondition prefix, minus the scene-data test and
  minus the lease" — never as a separate predicate.** Recording it as independent invites a
  later "dedup" that either hands grading a lease it must not need or drags the scene-data
  test onto the grading path; both are behavior changes.

  > **Consequence for the ADR's own text.** The claim elsewhere in earlier revisions that
  > color grading is *"deliberately exempt from all three predicates / always applied
  > per-instance"* is **wrong as stated**. Grading is exempt from the **lease (P1)**, from
  > **P2** and from **P3** — it is **not ungated**. It is gated by P4, and there is a second,
  > distinct **director-registration ordering guard** at `cpp:1897-1904`: the push is
  > attempted against `GaussianSplatSceneDirector::update_instance_color_grading(...)`
  > (`:1898`) and, if the director does not yet know this node, `:1900-1904` returns `false`
  > **without clearing** `grading_pushed_for_current_data` / `grading_explicit_pending`, so a
  > later replay retries once `register_instance_in_director` has landed. An implementer who
  > reads "ungated" and deletes either guard reintroduces the peer-clobbering bug the code
  > comment at `:1892-1895` describes. **The frozen matrix is P1 / P2 / P3 / P4 + the
  > director-registration ordering guard.**

  Three consequences the decomposition **must** preserve, none of which follows from a
  single lease:

  1. **P2 is not P1.** The four `show_*` debug flags are not merely dropped when the
     renderer is shared — they are **actively forced to `false` even for the lease-holding
     owner** (`helpers.cpp:608-611`, `shared_renderer ? false : owner.show_*`). A
     lease-gated setter cannot express "forced off for the owner too."
  2. **The painterly setters gate on P2 alone, with no ownership check**, and they
     `return` **before writing the node-local member** (`cpp:1026-1029`, `1039-1042`,
     `1051-1054`, `1068-1071`, `1081-1084`) — so the property value is discarded, not just
     the renderer write. `set_enable_painterly` (`cpp:1011-1024`) is the asymmetric
     exception: no P2 gate at all.
  3. **`_validate_property` keys off P2 alone** (`cpp:518-536`), never P1. So on a shared
     renderer the *owner's* inspector rows are hidden too. Deriving hiding from
     `holds_settings_lease()` — as an earlier revision of this ADR proposed — would make
     the owner's rows **reappear**. That is a behavior change, not a refactor.

  Additionally, **P1's "check" is itself a mutation**: `can_apply_renderer_settings()` is
  declared `const` but releases (`:1319`) and claims (`:1324`) entries in the global map on
  every call. And color grading is deliberately **exempt from P1/P2/P3**
  (`helpers.cpp:1403-1410`: *"No shared-renderer gate needed: peers no longer share a
  single color_grading slot"*) — but it travels an **independent push path**
  (`cpp:1884-1912`) that is **gated by P4 and by the director-registration ordering guard**,
  as set out above. "Exempt from the three predicates" is accurate; "ungated" is not.

  The hiding/gating string lists are kept **manually** in sync — `_validate_property`'s own
  comment points at `gaussian_splat_node_helpers.cpp:1284-1378`, which is already ~10 lines
  off from the real `can_apply_renderer_settings()` range (`:1294-1325`). The drift is
  measurable today, and §B1 records it rather than pretending the two lists agree.

- **C — color-grading replay flags.** `grading_pushed_for_current_data` and
  `grading_explicit_pending` (`gaussian_splat_node_3d.h:184-192`) form a two-track replay
  machine whose clear/set sites are prose-specified across five locations and *must not* be
  reset on EXIT_TREE (`cpp:396-404`, `1914-1953`, `helpers.cpp:1347-1358`). A wrong
  clear clobbers a peer's grading on a shared renderer.

- **D — `last_known_scenario` caching.** Cached at register time
  (`cpp:2443-2450`) precisely because `get_world_3d()` returns null by PREDELETE
  (`gaussian_splat_node_3d.h:209-214`). The dependency is prose-only.

These are the invariants #551 wants encoded in *types*, not comments.

### B1 — the gating matrix that must be carried forward verbatim

This table is **normative**. It is the current, verified behavior at the base SHA, and it is
the acceptance criterion for every step that touches settings. A step may change *where* a
predicate is evaluated; it may **not** change *which* predicate governs a property, or what
denial does. Any intended change is a separate, owner-approved behavior PR — never a
side effect of decomposition.

| Property / write | Gated by | Denial effect | Site |
|---|---|---|---|
| `set_max_splats` | P1 **then** P3 | dropped + `WARN_PRINT_ONCE` | `helpers.cpp:1385-1390` |
| `set_lod_enabled`, `set_lod_bias`, `set_lod_max_distance`, `set_frustum_culling`, `set_async_upload_enabled` | P1 | dropped silently | `helpers.cpp:1392-1397` |
| `set_painterly_enabled` (from `apply_renderer_settings`) | P1 @base — **P1 *and* P2 @master (#667)** | @base: dropped silently. **@master: forced to `false` when P2 holds** (`painterly_shared ? false : owner.enable_painterly`), with node-local state deliberately *not* cleared so the peer-set convergence hook can re-apply it when the node is alone again. | `helpers.cpp:1398`@base / **`:1415-1416`@master** |
| `set_painterly_edge_threshold/stroke_opacity/stroke_length/gamma` | P1 @base — **P1 *and* P2 @master (#667)** | @base: dropped silently. **@master: skipped entirely under `if (!painterly_shared)`.** | `helpers.cpp:1399-1402`@base / **`:1417-1421`@master** |
| `set_streaming_config_overrides` | P1 | dropped silently | `helpers.cpp:1454` |
| `debug/overlay_opacity`, `debug/debug_draw_mode`, `debug/runtime_preview` | P1 **only** | **renderer write** dropped silently — but the **node-local member is assigned first and always persists** (`helpers.cpp:697`, `:710`, `:729`, *before* the gates at `:699`, `:712`, `:731`), as does the trailing `update_gizmos()`. Not hidden in inspector. | assigns `helpers.cpp:697`, `:710`, `:729`; gates `:699`, `:712`, `:731` |
| `show_tile_grid`, `show_density_heatmap`, `show_performance_hud`, `show_residency_hud` | P1 **and** P2 | renderer write dropped; **and forced to `false` for the owner too** when P2 holds | gates `helpers.cpp:631-632`, `646-647`, `661-662`, `752-753`; forcing `:608-611` |
| node-local `show_*` member + `GaussianSplatSettingsManager` persistence | **ungated** — written before the gate | always applied | `helpers.cpp:627/629`, `642/644`, `657/659`, `748/750` |
| `edge_threshold`, `stroke_opacity`, `stroke_width`, `temporal_blend`, `painterly_seed` (setters) | P2 **only**, no ownership check | **node-local member write also skipped** | `cpp:1026-1089` |
| `enable_painterly` (setter) | **ungated at the setter** (asymmetric — deliberate today) | — | `cpp:1011-1024` |
| `color_variation` | no renderer control exists — explicit no-op | — | `cpp:1063-1066` |
| color grading — **write 1 of 3**: the push inside `apply_renderer_settings` | **P1-gated** (function-level early return), exempt from **P2/P3** only | not reached at all when P1 denies | `helpers.cpp:1403-1410`@base / `:1423-1429`@master, inside `GaussianSplatNodeRendererHelper::apply_renderer_settings()` which opens at `:1364` and early-returns at `:1368` on `!can_apply_renderer_settings()` |
| color grading — **write 2 of 3**: the independent push path | **P4** + the ordering guard below; exempt from P1/P2/P3 | push returns `false`; replay flags stay armed so a later replay retries | P4 `cpp:1880-1882` (`_can_push_color_grading_to_renderer()` at `cpp:1871-1873`); path `cpp:1884-1912` |
| color grading — **write 3 of 3**: the registration-time seed | gated **only** on `in_tree && in_world` (`cpp:2408-2410`) — not P1, not P4, not the ordering guard | no push | `director->update_instance_color_grading(...)` at `cpp:2463`, inside `_register_instance_in_director()` (opens `:2396`) |
| color grading — director registration not yet done | **ordering guard** (distinct from P4) | push returns `false` **without clearing** `grading_pushed_for_current_data` / `grading_explicit_pending` | `cpp:1897-1904` (rationale comment `:1900-1903`) |
| camera transform / projection | **ungated**, every node, every frame | — | `cpp:1590-1591` (this is #550, and Step 6 is the *only* step licensed to change it) |
| `_validate_property` inspector hiding | P2 **only** | hides `painterly/*`, the four `debug/show_*`, `quality/lod_bias`, `quality/max_splat_count` — **including for the owner** | `cpp:518-536` |
| sharing-status change detection | per-frame poll, not an event | re-runs `_apply_renderer_settings()` + `notify_property_list_changed()` | `cpp:1677-1686` (`shared_renderer_multi_instance_state`, decl `h:239`) |

**Known pre-existing inconsistencies — preserve them, or fix them in a named separate PR.**
The decomposition must not silently "clean these up," because each is observable:

- Hidden but not P2-gated: `painterly/enabled`, `quality/lod_bias`, `quality/max_splat_count`
  (their setters have no P2 gate, so a shared-renderer owner can still set them from GDScript
  and the write lands).
- P1-gated but never hidden: `debug/overlay_opacity`, `debug/debug_draw_mode`,
  `debug/runtime_preview`, `quality/max_render_distance`, `rendering/frustum_culling`.
- `quality/preset` stays editable while the `lod_bias` / `max_splat_count` it drives are
  hidden — a hidden knob remains indirectly movable.
- `_is_renderer_shared_with_other_content` is **duplicated** with two signatures
  (`helpers.cpp:97-111` taking `GaussianSplatNode3D &`, `cpp:41-58` taking
  `const Ref<GaussianSplatRenderer> &`). Collapsing the duplicate to one function is in scope
  for Step 2 and is behavior-preserving; changing its *predicate* is not.
- Only **one** drop in the entire surface warns (`helpers.cpp:1387`). Every other denial —
  the whole `apply_renderer_settings` body, all eight debug setters, all five painterly
  setters — is completely silent. Making denials observable is desirable but is a
  **behavior change**: it belongs in its own PR with its own sign-off, not inside a
  decomposition step.

## Decision

Replace friend-decomposition with **owned collaborators**: small C++ objects that own their
own state (private members), expose a **narrow, explicit contract**, and are **not friends**
of the node. The node becomes a thin `Node3D` scene-facing shell that (a) registers the
GDScript API, (b) routes property/notification plumbing, and (c) wires collaborators together
by passing values across contracts — never by reaching into another object's privates.

### Target collaborators (owned members, no `friend`)

| Collaborator | Owns (moves off the node) | Contract (illustrative) | Closes concern(s) |
|---|---|---|---|
| **`AssetBinding`** | `splat_asset`, `runtime_asset`, `renderer_data`, `asset_loading`, import flags, local AABB | `bind(asset)`, `set_procedural(arrays…)`, `reload()`, `clear()`; returns `PayloadView{ count, aabb, is_2d, origin_label }`; signals `data_ready` / `data_cleared` to the shell via a callback interface | 1, 2 |
| **`resolve_quality()` (pure core) + quality shell** | *core owns no state*; the shell keeps `quality_preset`, `lod_bias`, `max_render_distance`, `max_splat_count`, `lod_config`, `streaming_config`, `effective_config_snapshot` | **functional core:** `resolve(QualityInputs) -> ResolvedQuality` — a free function over an explicit input struct. **Imperative shell:** ProjectSettings reads, field assignment, cap-event diffing, and the two tail calls stay put. See §"Step 1 is a functional-core/imperative-shell split" | 3 (painterly params enter the core as **explicit inputs**, and stay owned by concern 4) |
| **`RendererRegistration`** | `renderer` `Ref`, `render_instance`, `gaussian_base`, `last_known_scenario`, director instance record, **the settings lease** | `attach(world)`, `detach()`, `publish_instance_params(InstanceParams)`, `try_apply(const RendererSettings&)` **gated by a held `RendererSettingsLease`**, `release_and_prune()` (see LifecycleGuard) | 5, 6 |
| **`ViewportRenderState`** | `cached_viewport_*`, `observed_viewport`, `viewport_texture_state`, deferral flags, observers | `track(viewport)`, `untrack()`, `is_ready()`, `render_target()`, `size()`; signals `became_ready`. **Camera publication removed** (see #550) | 7 |
| **`ColorGradingPolicy`** | `color_grading` + the two replay flags as a typed `GradingReplayState` | `set_grading(...)`, `on_resource_changed()`, `on_data_window_opened(director, id)`, `on_data_cleared()`, `on_renderer_ref_changed()` — named transitions replace prose (invariant C) | 10 |
| **`DebugOverlayController`** | 13 `show_*` flags, `debug_draw_mode`, HUD child nodes, settings-manager persistence | `set_show_*`, `sync_to_renderer(lease)`, `update_hud()` | 11 |

Concerns 9 (visibility), 12 (effectors/wind), 13 (property plumbing), 14 (stats), 15
(lifecycle dispatch), 16 (drag-drop) stay on the shell — they are genuinely the node's
scene-facing responsibility, and shrink to thin forwarders once 1/3/5/7/10/11 move out.

### Step 1 is a functional-core/imperative-shell split, not a policy object

> **Corrected in review round 2. The "pure `QualityPolicy`" premise was FALSE**, and Step 1
> was the ADR's lowest-risk, highest-value step — the whole staging order rested on it. An
> implementation agent dispatched to build it correctly refused on a gate check. Two
> independent reviewers then confirmed the defect at every anchor. What follows replaces the
> premise; it does **not** abandon the seam, because a real seam is still there.

**Why "pure" does not survive the code.** `GaussianSplatNodeQualityHelper::update_quality_settings()`
is not a computation — it terminates in renderer application and reads global config:

| What Step 1 must **not** drag into a "pure" object | Site |
|---|---|
| `owner._apply_renderer_settings()` — the direct entry into the gated apply path | `gaussian_splat_node_helpers.cpp:994-996` |
| …which reaches **P1** `can_apply_renderer_settings()` | `helpers.cpp:1368` → `:1294-1325` |
| …and **P3**, the splat-count heuristic + its lone `WARN_PRINT_ONCE` | `helpers.cpp:1385-1387` |
| …and the debug tail `apply_renderer_debug_settings()`, which evaluates **P1** *and* **P2** | `helpers.cpp:1456` → `:599` → P1 `:603`, P2 `:607` |
| `owner._update_instance_params_in_director()` — publishes to the director | `helpers.cpp:781` |
| `ProjectSettings` reads (tier preset + tier-budget opt-in) in `update_quality_settings` | `helpers.cpp:837-839` (used through `:841`) |
| `ProjectSettings` reads again in `apply_quality_tier_limits` | `helpers.cpp:1002`, `:1006-1007` |
| tree coupling — P1 reads `is_inside_tree()` / `is_inside_world()` | `helpers.cpp:1298` |

So the "pure" object would transitively evaluate **all three** predicates this ADR freezes,
touch the tree, read `ProjectSettings`, and publish to the director. It is also not
quality-owned: the LOD/streaming config builders read four **painterly** members
(`enable_painterly`, `painterly_seed`, `temporal_blend`, `edge_threshold`) whose setters are
**P2-gated with the node-local member write skipped** (invariant N4).

**The seam that does exist.** Split by *purity*, not by concern:

- **Pure core — `resolve(QualityInputs) -> ResolvedQuality`, a free function.** It receives
  an explicit `QualityInputs` struct and returns a value. It contains: the four preset
  tables, the `CLAMP` to `gs::GS_LOD_BIAS_MIN/MAX` and the `MAX(gs::GS_MIN_MAX_SPLAT_COUNT, …)`
  floor (`helpers.cpp:777-778`), the tier-cap `MIN` folds, the LOD ladder, the
  `GaussianSplatLODConfig` / `GaussianSplatStreamingConfig` struct construction, and the
  effective-snapshot construction. **The tier preset and tier-budget flag arrive as fields of
  `QualityInputs`, already read** — the core never calls `ProjectSettings`. **The four
  painterly values likewise arrive as explicit `QualityInputs` fields** — the core never
  reads a painterly member, so it cannot smuggle P2-gated state across the cut.
- **Imperative shell — stays on `GaussianSplatNodeQualityHelper`, at its current sites.**
  The `ProjectSettings` reads (`:837-839`, `:1002`, `:1006-1007`), assignment of the resolved
  values onto the node's fields, the cap-event diffing / WARN-on-cap logging against
  `previous_effective_snapshot`, and — **explicitly left where they are** — the two tail
  calls `owner._update_instance_params_in_director()` (`:781`) and
  `owner._apply_renderer_settings()` (`:995`).

**This wording is load-bearing.** "Extract `QualityPolicy`" would let a reviewer accept an
extraction that quietly drags the renderer write along with it. The acceptance test for Step 1
is therefore stated negatively as well as positively:

- The pure core's TU must not reference `GaussianSplatRenderer`, `GaussianSplatSceneDirector`,
  `ProjectSettings`, `Node`/`Node3D`, or any of P1/P2/P3/P4. A grep guard over the new TU's
  includes and symbols enforces this — that is what makes "pure" checkable rather than claimed.
- `helpers.cpp:781` and `helpers.cpp:995` are **unchanged by Step 1**. Their disappearance or
  relocation is a review blocker.

**Risk reclassification.** Because the shell edits stay inside the node/helpers TUs and the
core is a new pure TU, Step 1 is **R1** — but *only* under this split. Under the withdrawn
"policy object owns the concern" framing it would have been **R2**, because the seam then
crosses renderer-settings application. If the owner rejects the functional-core framing in
**D6**, Step 1 becomes R2 and the §B1 matrix test must be moved to gate Step 1 rather than
Step 2.

**Open sequencing problem this creates (for D3/D6).** The N1 evidence requirement is
circular for Step 1 as currently scheduled: the §B1 gating-matrix table test **does not exist
on master** (there is no `test_quality*` and no gating-matrix test), and this ADR schedules it
as the **Step 2** gate. Authoring it as a Step 1 prerequisite would mean an agent writing the
normative executable encoding of the §B1 matrix — which *is* the artifact **D5** asks the
owner to freeze. The test would become the de facto freeze, authored ahead of sign-off. The
functional-core split is what makes this tractable: the pure core needs only a preset/clamp
unit test, and the matrix test stays where it is. **This only works if D6 answers "core/shell".**

### Making the four invariants structural

- **Invariant A + D → `LifecycleGuard` / director atomic API (#551).** Add
  `GaussianSplatSceneDirector::release_renderer_and_prune(Ref<GaussianSplatRenderer> &r, const RID &scenario)`
  that performs `r.unref()` **then** `_prune_world_if_unused(scenario)` as one indivisible
  step, or an equivalent scoped `PruneAfterUnref` guard whose destructor enforces the order.
  Both node and world PREDELETE collapse to a single call; the ordering **cannot be expressed
  wrong** because the two operations are no longer separately callable at the site. `last_known_scenario`
  becomes a private of `RendererRegistration`, refreshed on attach. The mirrored 40-line
  comments shrink to a 3-line pointer at the guard.

- **Invariant B → `RendererSettingsLease` (P1) *plus* a preserved `SharingState` (P2) —
  two seams, not one.** (Corrected in review round 1. The earlier revision proposed a single
  lease and derived `_validate_property` hiding from `holds_settings_lease()`; per §B1 that
  would have unhidden the owner's inspector rows on a shared renderer and dropped the
  "forced to `false` even for the owner" semantics of the four `show_*` flags. Both are
  behavior changes and both are now forbidden.)

  - **P1 becomes `RendererSettingsLease`,** a typed token held by `RendererRegistration`,
    replacing the file-static map. Dead-owner stealing (`_settings_owner_is_live`,
    `helpers.cpp:38-44`) becomes `try_acquire`. Because today's `can_apply_renderer_settings()`
    mutates the map from a `const` method (`:1319`/`:1324`), the lease API must make the
    acquire/release **explicit and non-`const`**, with a separate pure
    `holds_lease()` query — this is the one place the refactor legitimately improves the
    shape without changing the predicate.
  - **P2 stays a distinct, separately-evaluated predicate** — a `SharingState` value
    (`instance_count > 1 || has_world_submission`) read from the director, exposed as
    `registration.sharing_state()`. The two duplicated implementations
    (`helpers.cpp:97-111`, `cpp:41-58`) collapse into this one function; the predicate is
    unchanged.
  - **P3 (the splat-count heuristic, `helpers.cpp:1385`) is carried forward verbatim** as a
    guard inside the `set_max_splats` path. It is *not* folded into the lease: it is a
    different question ("does the renderer already carry someone else's splat count?") and
    two peers with equal counts must continue not to trip it.
  - **`_validate_property` hiding continues to derive from P2 alone** — from
    `sharing_state()`, **never** from `holds_settings_lease()`. This keeps the owner's rows
    hidden on a shared renderer, exactly as today. The win is that the hide set at
    `cpp:526-532` becomes one named `constexpr` list (`kP2HiddenProperties`) instead of an
    inline string sequence, so drift becomes impossible; the *predicate* is untouched.
    **It is not shared with the gating sites** — see N8: the hide set and the P2-gated
    setter set are deliberately different, and there is no duplicated list to collapse
    (the gating sites are plain `if` guards in the setter bodies, not name lists).
  - **The `show_*` "force to `false` when shared" semantics (`helpers.cpp:608-611`) is part
    of the contract**, not an implementation detail: `DebugOverlayController::sync_to_renderer`
    takes both the lease *and* the `SharingState` and reproduces the forcing exactly.
  - **Color grading stays exempt from P1/P2/P3**, and the per-instance push path
    (`cpp:1884-1912`) is preserved as-is **including both of its own guards**: P4
    (`cpp:1880-1882`) and the director-registration ordering guard (`cpp:1897-1904`).
    `ColorGradingPolicy` must therefore receive P4 as an explicit precondition input — it
    must **not** be given the lease, and it must **not** acquire the scene-data-ownership
    test that distinguishes P1 from P4.

- **Invariant C → `GradingReplayState`.** The two bools become a small enum-driven type with
  named transitions (`ArmedExplicit`, `PushedForWindow`, `Idle`) inside `ColorGradingPolicy`;
  the "do not reset on EXIT_TREE" rule is encoded by simply *not exposing* a reset there.

### One-writer-per-frame camera publication (#550)

`_update_viewport_render_state` writes `renderer->set_camera_transform/projection` every
frame (`cpp:1590-1591`); with N nodes on a shared renderer that is N redundant
last-write-wins writes each frame — the same "renderer-wide state, per-node writer" ambiguity
the settings lease exists to prevent. **Move camera publication to a single writer**: a
`CameraPublisher` owned by `GaussianSplatManager` / `GaussianSplatSceneDirector` that reads
the active camera once per renderer per frame and publishes it. Nodes stop writing camera
state entirely; `ViewportRenderState` keeps only render-target tracking. This closes #550 and
removes camera from the node's per-frame surface.

## Staged migration (each step compiles and is CI-green on its own)

Ordered to extract zero-lifetime-risk policy first, encode the structural invariants next,
and touch GPU/lifetime seams last. Every step keeps the public GDScript API and serialized
property names byte-identical; behavior is preserved and proven per step.

> **Risk-label rule (corrected in review round 2).** The per-step labels below are the
> *expected* class; CI re-derives the class from the diff and uses the **higher** of the two
> (`agentic-engineering.md:88-90`). Two triggers matter here and were previously applied
> only to Step 3:
>
> - **`agentic-engineering.md:86` makes "public API/compat" an R3 trigger.** The node's
>   public surface is **38** `ADD_PROPERTY` registrations and **100** `ClassDB::bind_method`
>   bindings in `gaussian_splat_node_3d.cpp`, plus the `_set`/`_get` legacy-compat shim at
>   `cpp:576ff` (which still re-routes the unbound `painterly/color_variation`). **Any step
>   whose diff touches those lines is R3** — ADR-before-implementation, two reviews, and
>   CODEOWNER + human approval — regardless of the label printed below. N10 asserts these
>   stay byte-identical, so the intent is that *no* step trips this; a step that finds it
>   must trip it stops and re-declares rather than proceeding under an R1/R2 label.
> - **`:85` (R2) is the renderer/GPU/streaming/VRAM class.** A step that only moves C++
>   state between node-local TUs, with no renderer or GPU edit, is **R1**, not R2.

- **Step 0 — this ADR + a nested `nodes/AGENTS.md`** capturing the ownership seams and the
  four invariants, so new code does not re-widen the surface. *(R0, docs.)*
- **Step 1 — extract the pure quality *resolver*** — `resolve(QualityInputs) -> ResolvedQuality`
  as a free function in a new TU, leaving the imperative shell (ProjectSettings reads, field
  assignment, cap-event diffing, and **both** tail calls at `helpers.cpp:781` and `:995`) on
  `GaussianSplatNodeQualityHelper` at its current sites. See §"Step 1 is a
  functional-core/imperative-shell split" — the earlier "extract a pure `QualityPolicy` that
  owns the concern" framing was refuted and is withdrawn. It befriends nothing; painterly and
  tier inputs are injected explicitly. **Gate:** guard lane + new
  `tests/…/test_quality_resolve.h` (preset tables, tier-cap `MIN` folds,
  `GS_LOD_BIAS_MIN/MAX` clamps, `GS_MIN_MAX_SPLAT_COUNT` floor) + the purity grep guard over
  the new TU's includes/symbols + a diff check that `helpers.cpp:781`/`:995` are untouched.
  *(R1 **under this split only** — see D6; R2 if the owner restores the policy-object framing.)*
- **Step 2 — `RendererSettingsLease` (P1) + `SharingState` (P2) + preserved P3** (invariant B).
  Replace the free-function owner map with the lease; collapse the two duplicate
  `_is_renderer_shared_with_other_content` implementations into one `sharing_state()`;
  keep `_validate_property` hiding derived from **P2**; keep the P3 splat-count heuristic on
  the `set_max_splats` path; keep the `show_*` force-to-`false`-when-shared semantics.
  Behavior-preserving in the strong sense of §B1.
  **Gate (blocking):** a **gating matrix table test** that asserts the §B1 table cell-by-cell —
  for each property, on each of {sole node, shared-renderer owner, shared-renderer non-owner,
  dead-owner-stolen}, assert (a) whether the renderer write lands, (b) whether the node-local
  member is written, (c) whether the inspector row is hidden. This test must be written
  **against Step 2's own base first and pass unmodified there**, then still pass on the head —
  that is what makes "behavior-preserving" checkable rather than asserted.

  > **⚠ Step 2 is BLOCKED on the §B1 re-freeze (round 3).** "Its own base" is deliberately
  > *not* the SHA recorded at the top of this document. #667 changed P2 painterly gating after
  > that SHA (see the anchor-status note), so a matrix test authored from the rows in §B1 as
  > they stand would encode pre-#667 semantics, pass against the recorded base, and **fail on
  > head** — an unsatisfiable acceptance criterion, and one that would pressure an agent to
  > "fix" the test rather than the matrix. Step 2 therefore may not start until §B1 has been
  > re-derived against a current base and Decision D5 re-confirmed against the new behavior.
  > Once that re-freeze lands, the base referred to here is the re-freeze base. Plus the existing
  lifetime tests and a lease unit test (acquire / steal-dead-owner / release / peer-denied).
  *(R2.)*

  **Fixture recommendation (robustness — explicitly NOT a fix for a vacuous test).** Set
  `enable_painterly = true` and `quality_preset = QUALITY_CUSTOM` in the fixture, assert those
  preconditions in the test body, and assert **both** polarities per row (P2 false → visible,
  P2 true → hidden).

  > **Why this is a widening, not a repair (review round 2).** An earlier review comment
  > claimed a matrix test on default node state *"passes identically with the entire P2 branch
  > deleted."* **That claim was retracted and must not be propagated — it is false.** On
  > default state (`enable_painterly = false`, `h:150`; `quality_preset = QUALITY_BALANCED`,
  > `h:136`), **5 of the 7 clauses in the P2 branch still discriminate**, so deleting the P2
  > branch *would* fail a default-state matrix test:
  >
  > - **`painterly/enabled`** — the `!enable_painterly` rule at `cpp:539-543` explicitly
  >   exempts it: `p_property.name.begins_with("painterly/") && p_property.name != "painterly/enabled"`
  >   (`cpp:540`). Hidden by the P2 branch **alone**.
  > - **The four `debug/show_*`** — the only other rule touching `debug/` is `cpp:569-573`,
  >   wrapped in `#ifndef DEBUG_ENABLED`. CI builds `target=editor`, and `SConstruct:495`
  >   sets `env.debug_features = env["target"] in ["editor", "template_debug"]`, which at
  >   `SConstruct:511-513` appends `DEBUG_ENABLED`. So that rule is **compiled out** in the
  >   test binary and these four are hidden by the P2 branch **alone**. *(Note: `DEBUG_ENABLED`
  >   follows from `target=editor`, **not** from `dev_build=yes` — `dev_build` sets
  >   `DEV_ENABLED` at `SConstruct:516-518`. The conclusion is unchanged; the attribution in
  >   the retraction comment was itself imprecise.)*
  >
  > Only `painterly/{edge_threshold, stroke_opacity, stroke_width, temporal_blend, seed}`
  > (shadowed by `cpp:539-543`) and `quality/{lod_bias, max_splat_count}` (shadowed by
  > `cpp:561-566`, since the default preset is not `QUALITY_CUSTOM`) are masked on defaults.
  >
  > The fixture is therefore worth adopting because it widens coverage from **5** discriminating
  > clauses to all **7**, and guards against a future default-value change silently hollowing
  > the test. It is **not blocking**, and it must not be described as fixing a test that proves
  > nothing.
- **Step 3 — `release_renderer_and_prune` / `PruneAfterUnref`** (invariant A, **#551**).
  Rewrite **both** node and world PREDELETE to the single atomic call; delete the mirrored
  comments. **Gate (blocking):** `tests/…/test_renderer_lifetime_proof.h` scenario_c (F6
  reload) stays green + a compile-time proof the two ops are not separately callable at the
  site; runtime lifetime evidence attached. **R3 — separate owner sign-off.**
- **Step 4 — `ColorGradingPolicy` / `GradingReplayState`** (invariant C, 10). Behavior-preserving;
  grading + shared-renderer peer tests. *(R2.)*
- **Step 5 — `ViewportRenderState` + `AssetBinding` as owned (non-friend) members** (7, 1, 2).
  Drop the `friend` declarations for the viewport/asset helpers. **Gate:** editor-preview
  smoke + new node+`splat_asset` integration coverage (closes the #299 gap). *(R2.)*
- **Step 6 — one-writer camera publication** (**#550**). Move `set_camera_transform/projection`
  into the manager/director `CameraPublisher`. **Gate:** visual validation on GrandmasHouse
  (shared renderer, multi-node) — rendering-math-adjacent, must validate on real-scan content.
  *(R2.)*
- **Step 7 — `DebugOverlayController` + shrink the `friend` block** (11). Remove the
  `friend class …` grants (`gaussian_splat_node_3d.h:123-128`) for every helper that
  preceding steps converted into an owned collaborator — Asset + Viewport (Step 5), Debug
  (this step), Renderer (Step 2). **The Quality shell and Visibility helper are *not*
  converted by any scheduled step** (Step 1/N14 pins the former at its current sites;
  concern 9 keeps the latter on the shell), so how many grants remain after this step is
  **decision D7**. Verify the header no longer exposes privates to the converted helpers.
  Node header becomes a thin shell. *(R1.)*

## Issue-closure mapping

| Issue | Disposition | Closed by |
|---|---|---|
| **#552** god-class decompose | **Closed** by the full sequence (Steps 1–7): friends → owned collaborators, real ownership seams | Steps 1–7 |
| **#551** PREDELETE ordering structural | **Closed** — the ordering becomes an atomic director API / scoped guard | Step 3 |
| **#550** N per-frame camera writers | **Closed** — one writer per renderer per frame | Step 6 |
| **#558** script-instantiable internals | **Advanced, not regressed** — every new collaborator is a plain internal C++ type (never `GDREGISTER`'d), so decomposition adds **zero** script surface; the heavy-internals `GDREGISTER_CLASS` audit (`register_types.cpp:116-120`) remains its own follow-up | (non-regression note) |
| **#516 / #517** world-node lifecycle | **Not closed here** — owned by in-flight **PR #578**. Step 3's guard should be reused by the world node; sequence Step 3 to land before/after #578 so only one of them touches `gaussian_splat_world_3d.cpp:112-166` | coordination |
| **#357 / #547** settings ownership / loader dedup | **Adjacent** — Step 2's lease clarifies renderer-settings ownership; routing `QualityPolicy` (Step 1) through `gs::settings` helpers is the natural home for #547's dedup. Tracked separately | follow-up |
| **#299** node+asset integration gap | **Closed** as the Step 5 gate | Step 5 |
| **#592** editor preview worker hang | **Out of scope** — different file (`editor/gaussian_resource_preview_generator.cpp`); noted for adjacency only | — |

## Consequences

- **Benefit.** State aliasing shrinks from 7 co-owners of **87** fields to one owner per seam
  with a narrow contract. Three prose-guarded invariants (A/B/C) become types that cannot be
  expressed wrong; the two mirrored 40-line comments are deleted. The quality **resolver**
  becomes unit-testable without a tree, a renderer, `ProjectSettings` or a GPU — note this is
  the pure core only; the gated application path deliberately stays on the shell. The node
  header stops leaking 87 privates.
- **Cost / risk.** Step 3 is R3 (GPU teardown ordering) and needs runtime lifetime evidence;
  Step 6 needs GPU visual validation (agents cannot raster locally — must run on the GPU
  runner). The refactor is behavior-preserving, so its value is velocity + correctness
  durability, not user-visible change — reviewers should grade it on *seam quality*, not diff
  size.
- **Coordination.** Step 3 and PR #578 both touch the world node PREDELETE; whichever lands
  second rebases onto the other. No step bundles unrelated work; each is independently
  reversible.

## Invariant list — what every step is graded against

Checkable. A step that violates one is rejected even with green CI.

| # | Invariant | How it is checked |
| --- | --- | --- |
| **N1** | The §B1 gating matrix holds cell-for-cell. No property changes which predicate governs it; no denial changes its effect (dropped / forced-off / node-local-also-skipped). **The matrix graded against is the re-frozen one** — see the round-3 anchor-status note and the Step 2 block; the rows as currently written predate #667. | The Step 2 gating matrix table test, written against **Step 2's own (post-re-freeze) base** and passing unmodified on head. |
| **N2** | `_validate_property` hiding derives from **P2 (`sharing_state()`) only** — never from lease-holding. The owner's rows stay hidden on a shared renderer. | Explicit case in the matrix test: shared-renderer **owner** → rows hidden. |
| **N3** | The `show_*` flags are forced to `false` when P2 holds, **including for the lease holder**; the node-local member and settings-manager persistence still receive the write. | Matrix test rows (a)+(b) for the four `show_*` flags. |
| **N4** | The painterly setters gated on P2 continue to skip the **node-local member write**, not just the renderer write. `set_enable_painterly` remains ungated at the setter. | Matrix test row (b) for the five setters + the asymmetry case. |
| **N5** | P3 (splat-count heuristic) remains a distinct guard: two peers with **equal** splat counts do not trip it. | Unit test with two equal-count peers asserting the write lands and no WARN fires. |
| **N6** | Color grading has **three** write paths and they are not interchangeable (§B1). (i) The push inside `apply_renderer_settings` is **P1-gated** by that function's early return (`helpers.cpp:1368`) and exempt from **P2/P3 only** — it must stay behind that return. (ii) The independent push path is exempt from P1/P2/P3 and keeps **both** of its own guards: **P4** (`cpp:1880-1882`) and the director-registration ordering guard (`cpp:1897-1904`); P4 is never replaced by P1 and never acquires P1's lease or scene-data test. (iii) The registration-time seed (`cpp:2463`) is gated only on `in_tree && in_world` and must not silently acquire or lose a gate. No step may collapse the three into one path. | Shared-renderer peer grading test unchanged, **plus** (a) a case asserting a push attempted before `register_instance_in_director` returns `false` and leaves `grading_pushed_for_current_data` / `grading_explicit_pending` armed for replay, and (b) a P1-denial case asserting the `apply_renderer_settings` grading push does **not** fire while the registration seed and the independent path still behave as today. |
| **N7** | Exactly one definition of the P2 predicate exists after Step 2 (the duplicate is deleted, not forked). | Grep guard: one `sharing_state()` definition; zero `_is_renderer_shared_with_other_content`. |
| **N8** | **Two** canonical lists, each defined exactly once, and they are **not** required to be equal: `kP2HiddenProperties` — the `_validate_property` hide set (`cpp:526-532`: every `painterly/*`, the four `debug/show_*`, `quality/lod_bias`, `quality/max_splat_count`) — and `kP2GatedPainterlySetters` — the five setters that gate on P2 (`cpp:1027`, `:1040`, `:1052`, `:1069`, `:1082`). Their asymmetry (`painterly/enabled`, `quality/lod_bias`, `quality/max_splat_count` are hidden but **not** P2-gated; `set_enable_painterly` is ungated) is the pre-existing inconsistency frozen at §B1 and must survive verbatim. Requiring a single shared list would force one of those two behavior changes and is forbidden. | Grep guard: each list literal appears exactly once, and a matrix-test case asserting a property that is hidden-but-ungated stays hidden **and** still writes through its setter. |
| **N9** | The PREDELETE unref-then-prune ordering (invariant A) cannot be expressed wrong at the call site — the two operations are not separately callable there. | Step 3: compile-time proof (the separate ops are private/unavailable at the site) + `test_renderer_lifetime_proof.h` scenario_c green. |
| **N10** | The public GDScript API and every serialized property name are byte-identical across all seven steps. | Property-list snapshot test; doc_classes completeness guard stays green. |
| **N11** | No new script surface: every collaborator is a plain internal C++ type, never `GDREGISTER`'d. | Grep guard on `GDREGISTER_CLASS` count in `register_types.cpp` (unchanged). |
| **N12** | Friendship strictly decreases. **The end-state count is open — see D7:** Steps 1–7 as scheduled migrate four of the six grants at `gaussian_splat_node_3d.h:123-128`, and the Quality shell + Visibility helper are deliberately left on the shell by Step 1/N14 and concern 9 respectively. "Zero after Step 7" is therefore **not** satisfiable as staged and must not gate Step 7 until D7 resolves it. | Grep guard, monotonically non-increasing (this half holds under either D7 option). |
| **N13** | Camera publication changes **only** in Step 6. No earlier step alters `cpp:1590-1591` semantics. | Diff review per step. |
| **N14** | The Step 1 pure core stays pure: its TU references no `GaussianSplatRenderer`, `GaussianSplatSceneDirector`, `ProjectSettings`, `Node`/`Node3D`, and none of P1/P2/P3/P4. The two tail calls at `helpers.cpp:781` and `helpers.cpp:995` remain at their current sites. | Grep guard over the new TU's includes/symbols + a per-step diff assertion that those two lines are unmodified. |

## Evidence a step must produce

1. **Gating matrix evidence (N1–N6):** the matrix test output from the base SHA and from the
   head, attached and diffed. This is the single most important artifact in this ADR — the
   defect it guards against (silently changing which predicate governs which property) is
   invisible to every other check.
2. **Guard lane:** `run_module_tests.py --guard-only` green, plus the N7/N8/N11/N12 grep
   guards, which land with the step that makes them true.
3. **Targeted tests:** existing node, lifetime, grading, and shared-renderer suites green
   **without modification**. Modifying an existing assertion is a review blocker absent a
   written reason. **"Green" is not sufficient — report the executed case count.** Many of the
   node suites that cover exactly the shared-renderer / settings-owner / P2-hiding / grading
   cases are tagged `[RequiresGPU]` and fall in the 26-case `[SceneTree]+[RequiresGPU]` set
   deferred by **#329**. `tests/ci/run_gpu_harness.py:42-46` states it keeps
   *"Intentionally no catch-all `*[RequiresGPU]*` batch"* and that tests outside a named batch
   *"stay invisible until they're added explicitly"*; `:36-41` records that unmatched batches
   *"report '0 tests matched' which the supervisor treats as success (advisory)"*. A suite
   that executed **zero** cases therefore satisfies "green without modification" while proving
   nothing. Every step citing this item must attach an executed-count > 0, and
   the sequencing question of whether #329's batch must land before Step 2 is an owner call
   (see D3).
4. **Step 3 (R3):** runtime lifetime evidence — RID counts across an F6 reload cycle, with
   `test_renderer_lifetime_proof.h` scenario_c green, plus the compile-time ordering proof.
   Separate owner sign-off.
5. **Step 6 (R2, rendering-adjacent):** visual validation on real-scan content
   (GrandmasHouse, shared renderer, multi-node) from the GPU runner. Agents cannot raster
   locally; absent runner output this reports "not run", never "passed".
6. **Step 5:** editor-preview smoke + the new node+`splat_asset` integration coverage (#299).
7. **Base anchoring:** base SHA recorded; `file:line` anchors re-verified against it.

No step weakens a guard, baseline, or threshold. A behavior change discovered to be
*necessary* is lifted out into its own PR with its own sign-off — it never rides inside a
decomposition step.

## Decisions the owner needs to make

- **D1 — Adopt "ownership seams, not friends" as the target** (owned collaborators, no
  `friend`), per #552's fix direction? (Y / N / amend.)
- **D2 — Approve the four invariants becoming structural** (LifecycleGuard, RendererSettingsLease,
  GradingReplayState, scenario-cache ownership)? In particular, **prefer the director-side
  atomic `release_renderer_and_prune` over a scoped `PruneAfterUnref` guard**, or vice versa?
- **D3 — Confirm the staged order and that Step 3 is R3** with its own sign-off + lifetime
  evidence gate, and Step 6 requires GPU visual validation.
- **D4 — Sequence Step 3 against PR #578** (which lands first for the shared world-node
  PREDELETE edit)?
- **D5 — Confirm the §B1 gating matrix is frozen for the decomposition.** The predicates
  (P1 lease / P2 sharing / P3 splat-count heuristic / **P4 grading precondition** + the
  director-registration ordering guard) and the listed pre-existing inconsistencies are
  carried forward **verbatim**, and every fix to them is a separate, separately-approved
  behavior PR. In particular, confirm we do **not** take the tempting simplifications:
  collapsing P2 into the lease, deriving inspector hiding from lease-holding, making the
  silent denials warn, or **"deduplicating" P4 into P1** (they share a three-check prefix but
  P4 must not gain the lease or the scene-data test). (Recommended: yes, freeze — each of
  those is user-visible.)

- **D6 — Confirm Step 1 is a functional-core/imperative-shell split** (pure
  `resolve(QualityInputs) -> ResolvedQuality`; ProjectSettings reads, field assignment,
  cap-event diffing and **both** tail calls stay on the shell at `helpers.cpp:781` and
  `:995`), rather than the withdrawn "`QualityPolicy` object owns the concern" framing?
  This is the decision that determines Step 1's risk class and whether the §B1 matrix test
  must move earlier:
  - **Core/shell (recommended).** Step 1 stays **R1** and independently landable; its gate is
    a preset/clamp unit test plus the N14 purity guard; the §B1 matrix test stays as the
    Step 2 gate, so no agent has to author the normative matrix encoding ahead of the D5 freeze.
    Cost: the quality concern is split across two artifacts rather than owned by one object,
    so "one owner per seam" is achieved for *state* but not for the apply path.
  - **Policy object (withdrawn but available).** Step 1 becomes **R2**, the §B1 matrix test
    must be authored to gate Step 1 instead of Step 2, and D5 must be answered *before* Step 1
    starts — because the cut then lands directly on P2-gated painterly state and the matrix
    test becomes the de facto freeze. Cost: the whole slice reworks if D1/D5 come back `amend`.

  Related: **"zero friends" is a weak success metric for D1** — it is satisfiable by widening
  the public surface instead of by shrinking aliasing. Consider grading D1 on
  *field-reachability* (fields × objects that can reach them) with one writer per moved field,
  and keep N12's friend-count guard as a floor rather than the target.

- **D7 — N12 ("zero friends after Step 7") is not satisfiable by Steps 1–7 as scheduled.
  Decide how to close the gap.** There are six friend grants at
  `gaussian_splat_node_3d.h:123-128`. The steps cover four of them: Asset + Viewport in
  Step 5, Debug in Step 7, Renderer implicitly in Step 2. **Two are never migrated, and in
  both cases that is deliberate elsewhere in this document:**
  - `GaussianSplatNodeQualityHelper` — Step 1 explicitly keeps the imperative shell *"on
    `GaussianSplatNodeQualityHelper`, at its current sites"*, and **N14** pins
    `helpers.cpp:781` (`owner._update_instance_params_in_director();`) and `helpers.cpp:995`
    (`owner._apply_renderer_settings();`, declared private at `gaussian_splat_node_3d.h:330`)
    as unmodified. The shell also writes `owner.lod_bias` / `owner.max_splat_count`
    (`helpers.cpp:777-778`) and `owner.effective_config_snapshot`. Friendship is **required**
    by the Step 1 design that D6 recommends.
  - `GaussianSplatNodeVisibilityHelper` — concern 9 stays on the shell, and the helper
    reaches privates at `helpers.cpp:1209-1224` (`owner.parent_visibility_target`,
    `owner.parent_visible`, `callable_mp(&owner, &GaussianSplatNode3D::_on_parent_visibility_changed)`)
    and `:1288` (`owner.update_mode`, `owner._update_visibility()`).

  So Step 7's precondition — *"once all six helpers are owned collaborators"* — is
  established by no step, and an agent following the staging literally will either fail the
  N12 gate or widen public accessors purely to delete two `friend` lines, which is the
  failure mode the D6 note above already warns about. **Options:**
  - **(a) Add the migration work.** A new step converts QualityHelper and VisibilityHelper
    into non-friend owned collaborators. This must be reconciled with N14's pin on
    `helpers.cpp:781`/`:995` — i.e. D6's core/shell split and a non-friend QualityHelper are
    in direct tension, and choosing (a) means revisiting one of them.
  - **(b) Scope N12 to what the steps actually deliver.** *"Friendship strictly decreases; at
    most two grants (Quality shell, Visibility) remain after Step 7, tracked by a named
    follow-up issue."* Correct Step 7's "all six helpers" wording accordingly. This is
    consistent with the D6 note that friend-count is a floor, not the target — but it does
    mean N12 stops being the clean gate it currently reads as.

  This ADR does **not** pick one: (a) changes the scope of the decomposition and collides
  with D6/N14, (b) changes what N12 asserts. Both are owner calls. **Until D7 is answered,
  N12 must not be treated as a Step 7 blocker**, because no scheduled step can satisfy it.
