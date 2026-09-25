# Known Public Alpha Limitations

This page is intentionally required by the renderer release-gate manifest. A
public-alpha candidate may carry an accepted limitation only when the candidate
evidence bundle points to a section here with:

- the issue URL;
- user-facing impact;
- affected platform or workflow;
- mitigation or unsupported-path statement;
- evidence proving the limitation does not hide renderer correctness failures.

## How to read this page

Every entry is a defect that is real, reachable in a supported configuration, and
shipping anyway, with the reason and a workaround where one exists. An entry here is a
decision, not an oversight.

**Blocking defects are not listed as limitations here** — they are in the
[acceptance bar](../governance/release-acceptance-bar.md)'s §11 list. This page is for
what we ship knowing about.

The one exception is the clearly-fenced **"Proposed, not yet accepted"** section at the
bottom. A defect lands there when someone has proposed shipping it but no named human has
accepted it yet, so it is **still a blocker**. It is written up in advance only so the
disclosure and the disposition are drafted together and cannot drift; nothing in that
section may be cited as an `accepted_alpha_limitation`. If you are looking for what the
alpha actually ships with, read everything *above* that heading.

> That list is **human-maintained, and today the machine gate cannot see most of it.** The
> candidate gate's population is issues labelled `priority:P0`, `priority:P1` or
> `release blocker`; the §11 alpha blockers #851, #833 and #54 carry none of those (all
> three are `priority:P2`), so nothing automated stops a release on them. (#929 was a
> fourth until it closed on 2026-09-20, carrying only `program:prod-ready`.) Read "it is in
> the blocker set" as "a human has to hold the
> release for it", not as a guarantee the tooling enforces. Labelling them is tracked as
> an obligation on the bar.

**Where an entry has not been reproduced on hardware, it says so.** "Unproven" means
nobody has seen it happen, not that it does not happen; the code reading that produced it
is cited so you can check it yourself.

### Disclosure here is not the same as admission by the gate

The five bullets at the top of this page are the bar a limitation must clear to be cited
by a release candidate as an `accepted_alpha_limitation` — including the last one,
evidence that the limitation does not hide a renderer correctness failure. **Most entries
below do not clear it yet**, because they are code-reading findings with no reproduction:
#1018, #983 and #1005 say so in their own Status lines.

Those two things are deliberately kept apart. This page's job as a *user* document is to
disclose everything real that we know about, reproduced or not; an entry with no
reproduction is still worth a user's time. Its job as a *gate* input is narrower, and an
entry that has not been reproduced **must not** be pointed at by a candidate's
`docs_path` until it has been. Nothing here should be read as having pre-cleared that
check. Do not relax the criterion at the top of this page to make an entry admissible —
produce the evidence, or leave the issue in the blocker set.

**And clearing it is still not sufficient.** #1025 is the one entry here with a named human
acceptance and hardware evidence behind all five bullets, and a candidate bundle *still*
cannot cite it: the gate additionally requires the issue to appear in the manifest's
`public_alpha_issue_ledger`, and #1025 does not
(`tests/ci/check_renderer_release_gates.py:1921-1924`). That entry is an R3 manifest edit,
tracked as [#1038](https://github.com/klausi3D/godotGS/issues/1038). Until it lands, this
disclosure is real for a reader and invisible to the machine — recorded here rather than
left to be discovered at tag time.

Verified against `b915afc51c5` (2026-09-17), **except where an entry names its own commit**.
The #1025 entry below is verified against `bc77ce31e9c` (2026-09-20): the behaviour it
describes postdates `b915afc51c5`, because #1026 landed after it, and checking that entry out
at the page-wide anchor would show the opposite.

## Rendering

### Splats trail by about 1.5 px under TAA while the camera or the content is moving ([#1025](https://github.com/klausi3D/godotGS/issues/1025))

With TAA enabled and the camera or the content in motion, splat detail is drawn behind its
true position by **≈1.5 px** — 0.27–0.87 frames stale, and 6–26× an in-frame geometry control
measured in the same frames on the two pans. (On a forward dolly it is smaller than that:
2× the control in frames, and 0.36 px against the control's 0.54 px.) The trail does **not**
grow with camera speed: **1.48 px** at a
15°/s pan and **1.65 px** at 56°/s, because TAA's variance clip is computed from the current
frame. With a *static* camera there is nothing to see — the splat projection has carried the
engine's temporal jitter since #929/#1026.

**Under FSR2 there is no ghosting, at either scale.** **0.000 frames** of staleness at scale
1.0 across a slow pan and a normal pan — exactly the floor the same in-frame control sets —
and **0.04 px** on a dolly; at scale 0.5 the splat trail is at or below the control's in two
of three motions. The reason is a mechanism nothing had written down: the composite already
feeds FSR2's reactive mask through the destination alpha, so FSR2 reconstructs splat pixels
from the current frame instead of accumulating them. **Do not avoid FSR2 on account of this
entry.**

**Affects:** Forward+, single view, with `use_taa = true`. Measured on Windows, RTX 3090,
Vulkan 1.4.325, on a real scan at 960×540.

**Workaround:** `use_taa = false` — already Godot's own default. FSR2 needs no workaround at
either scale, and neither does a still camera.

**Accepted** as a public-alpha limitation on 2026-09-20 under the acceptance bar's §10.1
exception
([disposition](https://github.com/klausi3D/godotGS/issues/1025#issuecomment-5752837553)).
The disposition record and the mechanism live in the bar's
[§8.1](../governance/release-acceptance-bar.md); this entry is the user-facing half.

**Status: measured, and the rig proved it could fail.** The
[measurement](https://github.com/klausi3D/godotGS/issues/1025#issuecomment-5751997279) ran on
both the pre-fix and post-#1026 binaries, which agree to within 0.01 frames in every cell, so
the jitter fix neither causes nor cures this. Three gates ran before any figure was reported:
two independent walks of each camera path with no temporal stage were bit-identical over all
16,588,800 pixels; the region masks were re-derived per camera pose by hiding each subject, so
they follow the content across the screen; and **every temporal config had to move a control
region relative to the no-temporal capture**, so a viewport that had silently refused TAA or
FSR2 could not have been reported as a clean result. That last gate is what this page's fifth
admission bullet asks for — evidence that the limitation is not hiding a renderer correctness
failure rather than merely not showing one.

**The determinism gate was clean in the matrix these figures come from, and not always.** An
earlier matrix on the same binaries failed it in 5 of 6 runs, by 3–9 LSB over 49–963 of those
16,588,800 pixels — four to five orders of magnitude below the signal, and it moves none of
the numbers above, but it means the GS pipeline is very nearly, and not exactly,
frame-deterministic under a moving camera. Anyone extending #1026's **exact-zero**
determinism assertion to a moving camera should expect it to be flaky, and decide what to do
about that before landing it rather than after.

**One honest caveat.** The trail is ≈1.5 px whatever the content, but its *visibility* is not
content-independent. On the soft real scan measured it is not visible without roughly 10×
amplification; a sharper, higher-contrast scan would show more, and that has not been
measured.

Fixing it means the module publishing a velocity field — which is *not* an engine-boundary
change, since the module already holds the buffers and the previous-frame camera — but a wrong
velocity smears confidently and reads as a renderer bug, which is why it is not alpha work.

### Transparent viewports are opaque under TAA or FSR2 ([#989](https://github.com/klausi3D/godotGS/issues/989))

A viewport with `transparent_bg` renders fully opaque when TAA or FSR2 is enabled. Alpha
is hardcoded to 1.0 in Godot's `taa_resolve.glsl` and in the FSR2 callbacks; a mesh-only
control shows identical loss with no splats present, so this is not splat-specific and
cannot be fixed from this module.

**Workaround:** do not combine `transparent_bg` with TAA or FSR2.

> **A warning for whoever fixes this.** The destination alpha #989 is about is **not** just
> an alpha channel — it is also, by accident, FSR2's reactive mask. Godot has no dedicated
> reactive texture: `params.reactive` is an alpha-swizzled view of the internal colour
> buffer, and the composite writes splat coverage into that alpha. That coupling is why
> splats do **not** ghost under FSR2 (see #1025). **Changing this channel changes FSR2's
> temporal behaviour silently**, with no test that would notice. Measure FSR2 ghosting
> before and after any #989 fix.

### Painterly rendering has no automated coverage, and its material cannot be assigned from a scene ([#997](https://github.com/klausi3D/godotGS/issues/997))

Painterly needs two things, and `painterly/enabled` is only one of them: a valid
`PainterlyMaterial` must also be set on the renderer, or the raster stage reports
`PAINTERLY_MATERIAL_UNAVAILABLE` and falls back to the baseline pipeline
(`renderer/render_pipeline_stages.cpp:2561-2567`).

There is no supported way to do that from a scene file. `painterly_material` is a property
of `GaussianSplatRenderer`, which is `RefCounted` rather than a `Node`, so it never appears
in the inspector; the only route is `node.get_renderer().painterly_material = ...` from
script. `GaussianSplatRenderer::set_painterly_material` has **no callers anywhere in this
repository**.

Worse, the shipped demo scenes look like they configure it and do not.
`scenes/testlevel.tscn` and `scenes/ancient_corinth.tscn` assign `painterly/material` on
`GaussianSplatNode3D` — a property that class does not bind — so Godot discards the
assignment silently at scene load.

The consequence for coverage: no in-repo test exercises the painterly GPU path. The
painterly test suite renders **neither** pipeline; it is a CPU software rasterizer
(`modules/gaussian_splatting/tests/test_painterly_pipeline.h:241-364`). The GDScript
painterly tests call `set_enable_painterly(true)` and assert only visible-splat counts, so
they pass on a baseline frame.

**Workaround:** treat painterly as experimental. Assign the material from script, confirm
you are actually on the painterly path before drawing conclusions, and validate visually in
your own scene.

### Painterly ignores pass parameters the baseline pipeline sets

`render_painterly_stage` hand-copies a subset of the render-parameter struct that
`render_baseline_stage` populates, so controls that work on the baseline path are silently
inert under painterly. This is a class of defect, not a single bug, and the instances below
are the ones found so far rather than the complete set.

- **Per-node wind freezes mid-sway**
  ([#1018](https://github.com/klausi3D/godotGS/issues/1018)). `wind_time_seconds` is never
  advanced under painterly (`interfaces/painterly_renderer.cpp`, params built around
  `:1647-1916`), so it stays at its `0.0f` default
  (`renderer/tile_render_types.h:534`) forever. A node using the
  `rendering/wind_override_enabled` force path therefore renders permanently deformed off
  its rest position instead of swaying. **Unproven:** this is a code-reading finding at
  `b915afc51c5`, not reproduced on a GPU — and per #997 it almost certainly has never been
  observed, because nothing in the repo can put the renderer on the painterly path from a
  scene file.
- **Lighting-mode fields** are tracked as
  [#851](https://github.com/klausi3D/godotGS/issues/851): with painterly enabled you get
  **black contour lines**, and `shadow_strength` is **inert** — resolve-time lighting
  (mode 0) does not receive what the baseline stage assigns it. That one is in the
  **blocker** set rather than here, but its symptom is named so a user who sees it can
  find the issue.

**Workaround:** none. Do not rely on wind, or on any per-pass control, while painterly is
active.

### The painterly composite's `blend_strength` is a no-op ([#1001](https://github.com/klausi3D/godotGS/issues/1001))

`push_constant.blend_strength` is assigned the literal `1.0f`
(`interfaces/painterly_renderer.cpp:1389`) with nothing feeding it. The shader does consume
it (`shaders/painterly_composite.glsl:84`), so the multiply happens — it is just an
identity. There is no user-facing control to set.

This is unrelated to `PainterlyMaterial.palette_blend_strength`, which is a different
quantity and does work.

**Workaround:** none needed; there is nothing to set. Do not expect a blend control.

## Scripting API

### `get_statistics()` can crash the engine when polled every frame ([#1030](https://github.com/klausi3D/godotGS/issues/1030))

`GaussianSplatNode3D.get_statistics()` is ClassDB-bound
(`nodes/gaussian_splat_node_3d.cpp:233`) and is the natural call for a per-node HUD. Called
**once per rendered frame** from GDScript it intermittently takes the process down with a
`CrashHandlerException` inside the call. It builds a large `Dictionary` out of live
metrics structures (`:1460` → `render_diagnostics_orchestrator.cpp`) that the render
thread is mutating concurrently under the default multi-threaded
`rendering/driver/threads/thread_model=2`.

**Workaround:** poll it at **4 Hz or slower** — roughly every 15th frame, which is the rate
the shipped overlay itself refreshes at. A 444-frame run at that rate did not crash.

**Status:** **reproduced, not diagnosed** — 2 crashes in 5 runs on an optimized editor
build; the C++ backtrace was unsymbolized, so the faulting function is not named and no
root cause is established. 2-of-5 is a rate, not a mechanism.

**And the rate itself is suspect.** Those runs were taken on 2026-09-17, inside a window
when a stray `godot.head.exe` left over from 2026-09-16 was holding this machine's GPU
continuously; it was not killed until 2026-09-19. A crash the issue attributes to
main-thread/render-thread contention is exactly the kind of defect a third process
competing for the GPU would make *more* likely, so **2-of-5 is plausibly an overestimate**
of what a user on a quiet machine would hit.

The same premise cuts the *other* way for the workaround: a 444-frame run that stayed clean
**while the hazard was elevated** is **stronger** evidence that 4 Hz is safe, not weaker.
(An earlier revision of this entry said "correspondingly weaker", which was simply the wrong
direction — both figures move the same way from the same premise.)

**The defect is real either way, and both numbers still need retaking on a quiet machine** —
an overestimate is not a measurement, and one clean run is not a safety proof.

## GaussianSplatWorld3D

### Any payload change costs a full resubmit — about 2.1 s at 1M splats ([#1008](https://github.com/klausi3D/godotGS/issues/1008))

Changing `gaussian_data`, `bounds`, `metadata`, `lod_bias`, `max_render_distance` or
`max_splat_count` all converge on `_register_shared_renderer()`, which rebuilds the entire
submission from scratch. There is no dirty-field tracking and no equality early-out, so a
one-field nudge pays the whole cost: a blocking render-thread round trip, then
`clear_gaussian_data()` followed by a full GPU re-upload of every splat buffer.

**Two bounds on that.** The re-upload is the **resident** branch — a world whose payload is
file-backed takes `set_file_backed_payload_source()` instead
(`renderer/render_data_orchestrator.cpp:799-803`), which does not re-upload the splat
buffers the same way. And a world that holds no registered submission resubmits nothing.
The cost below is the resident path, which is the one the alpha's world route uses.

Medians reported on the issue, on an RTX 3090 with an optimized build: 17 ms at 10k splats,
259 ms at 100k, **2124 ms at 1M**. Those figures are from #1008 and have not been
re-measured here; the code path is verified. It is not a per-frame cost — it is paid when
the payload changes.

**Treat the timings as an upper bound, not a measurement.** They were taken on 2026-09-17,
inside a window when a stray `godot.head.exe` from 2026-09-16 was holding this machine's
GPU continuously until it was killed on 2026-09-19. Another PR's figures taken in the same
window moved by roughly 10% when retaken on the quiet machine. The *shape* of the finding
does not depend on the numbers — a full rebuild with no dirty-field tracking and an
unconditional re-upload is a code fact, and the ~2 s order of magnitude at 1M is not
plausibly contention alone — but the three medians should be retaken before anyone quotes
them as a baseline.

**Workaround:** change world content at load boundaries, not during gameplay.

### An emptied world does not reach the renderer ([#1002](https://github.com/klausi3D/godotGS/issues/1002))

`GaussianSplatWorld::clear()` is the one payload mutator that emits no `changed` signal.
Every other one does. The director holds its own copy of the payload rather than the
resource, so with no signal nothing re-registers and the previous content stays on screen.
`clear()` is script-bound and has no in-tree C++ callers, so this is reachable only from
user code.

A second, related path: `get_metadata()` returns the resource's `Dictionary` by reference,
so mutating it from script changes the resource without emitting `changed` either.

**Workaround:** assign an empty `GaussianData` instead of calling `clear()`.

### Bounds are never re-derived after they are once set ([#1003](https://github.com/klausi3D/godotGS/issues/1003))

Bounds are derived from the payload only while they are empty. Once `bounds` has volume —
however it got there — a later payload assignment never re-derives it, and the stale value
becomes the culling AABB. Content relocated with its payload can be culled at the wrong
place.

**Workaround:** call `set_bounds()` explicitly after relocating a payload.

### `strict_identity_transform` is bypassed on every resubmit ([#1006](https://github.com/klausi3D/godotGS/issues/1006))

The transform check runs on every *apply* path and on no *resubmit* path — the resubmit
helper calls `_register_shared_renderer()` directly, which contains no check. So a world
node moved after a successful apply is republished unchecked by any of the parameter
setters. The node also never tracks transforms, deliberately.

**Workaround:** do not move a `GaussianSplatWorld3D` after applying it while
`strict_identity_transform` is on; the setting will not catch you.

### Applying a world writes `world_path` into your resource ([#1007](https://github.com/klausi3D/godotGS/issues/1007))

Every apply of a world that has a resource path injects a `world_path` key into the
resource's own metadata dictionary — and because metadata is returned by reference, the
director's record aliases the same dictionary. The write bypasses `set_metadata()`, so no
`changed` signal fires and the injection is invisible. Metadata is serialized, so a
`.gsplatworld` saved after an apply persists the key.

In-memory worlds with no resource path are not affected.

**Workaround:** none. Do not treat a world's metadata dictionary as exclusively yours.

### A resource replaced mid-load can stay connected ([#1005](https://github.com/klausi3D/godotGS/issues/1005))

`Resource::connect_changed` routes through the loader when called off the main thread
during a load, queueing the connection rather than making it; a matching
`disconnect_changed` on the main thread then finds nothing connected and is a silent no-op,
because it never reaches that queue. The queued connection is honoured afterwards anyway.
A world resource swapped inside that window can leave the old resource connected and
resubmitting.

**Status:** confirmed at code level; the reachable window is narrow and no end-to-end
reproduction was constructed.

**Workaround:** avoid replacing a world resource while it is still loading.

## Sorting

### A failed sorter grow can lose both sorters ([#983](https://github.com/klausi3D/godotGS/issues/983))

A tile-sorter grow retires the old sorter before the enlarged buffers are allocated. If
that allocation fails, both are lost, and the reduced-capacity fallback then churns every
frame.

**Status:** a code-reading finding, **not reproduced on NVIDIA hardware** — unproven, not
absent. It is strictly narrower than what #982 fixed.

**Workaround:** reduce splat count rather than raising the overlap-record cap.

## Platforms and packaging

### Linux is smoke-tested, not editor-tested

The Linux CI lane runs the `ply`, `pipeline`, `runtime` and `module` categories headless
under xvfb on a GPU-less hosted runner. It runs neither `qa` nor `sorting`, so no QA-scene
and no GPU-sorting evidence exists for Linux at all; every GPU-backed lane in this project
is Windows. See the [compatibility matrix](../reference/compatibility-matrix.md).

**Workaround:** none. Treat Linux as an evaluation platform.

### macOS is build-supported and unvalidated

The build accepts macOS and no lane exercises it. There is no macOS CI, no published macOS
binary, and no evidence of any kind.

**Workaround:** build from source and validate it yourself.

### Nightly Linux editors are unoptimized

Nightly Linux builds are `dev_build=yes` (`-O0`), which inflates CPU-side frame cost by
roughly an order of magnitude; the `.dev` segment in the filename is that flag. This is a
**nightly** property — a tagged release builds Linux without it.

**Workaround:** use nightlies to see GodotGS work, not to judge how fast it is. Build with
`target=editor optimize=speed_trace` for representative numbers.

### No Linux export template is attached to releases

The Linux export template is built and uploaded as a CI artifact, but it is not among the
files attached to a release — only the Windows template is. Exporting a Gaussian-Splatting
game on Linux therefore requires building the template yourself. See
[export templates](export-templates.md).

**Workaround:** build the Linux template from source.

### Nothing is code-signed

No published binary is signed, on any platform — there is no signing step anywhere in the
release workflows. Windows SmartScreen will warn on the editor and on games exported with
the GodotGS template.

**Workaround:** verify downloads against the published `.sha256` sidecars.

---

## Proposed, not yet accepted

**Everything in this section is still a blocker.** These are defects someone has proposed
shipping with, where no named human has accepted them yet. They are drafted here so the
user-facing disclosure and the acceptance-bar disposition are written together and cannot
drift apart. **Nothing here may be cited as an `accepted_alpha_limitation`** — a candidate
bundle that does so is wrong twice over, because the acceptance does not exist and because
the gate additionally requires a `public_alpha_issue_ledger` entry that does not exist
either. The machinery behind both conditions — and the reason a classification means less
than it looks — is in the acceptance bar's
[§9.1](../governance/release-acceptance-bar.md); it is deliberately not restated here.

### The bottom of the frame can go empty when a frame needs more overlap records than are allocated ([#54](https://github.com/klausi3D/godotGS/issues/54))

Every splat that touches a tile costs one **overlap record**. If a frame needs more records
than the renderer has currently allocated, the tiles with
the highest tile index get nothing — and because tiles are numbered row by row, that is a
**hard horizontal line across the frame with only background below it**, plus one
partially-truncated tile row at the seam. The line sits wherever the budget ran out, so it
moves as your camera moves.

Two limits matter, and they are not the same number:

- **The allocated capacity**, which the renderer sizes and resizes itself. It starts at
  roughly 50 records per visible splat, shrinks toward the measured demand when the scene
  needs less, and grows after a drop. On the default path it learns the demand from a
  readback of an earlier frame, so it is always a little behind.
- **The configured cap**, `rendering/gaussian_splatting/gpu_sorting/max_overlap_records`
  (default **100,000,000**). The allocated capacity never grows past it.

**At defaults you can see a brief band, not a lasting one.** A sudden jump in demand, such
as the camera moving quickly into a dense close-up, can outrun the allocated capacity. The
band then shows for a frame or a few and goes away as the capacity catches up. This case
follows from the code; it has **not** been captured on hardware, and how many frames it
lasts is not measured. A **lasting** band needs demand above the configured cap. The
measurements below produced it only by forcing that setting 200–1000× below its default.

**Status: reproduced on hardware 2026-09-22** — RTX 3090, Vulkan, `dev_build=yes` editor
binary at `6f4552076c7`. 100,000 splats filling a 512×512 viewport at `tile_size = 16`
demand **708,814 records**, 7.09 per splat. With `max_overlap_records` forced down:

| `max_overlap_records` | Last lit scanline (of 512) | Empty tiles (of 1024) |
| --- | --- | --- |
| 100,000 | 95 | 837 |
| 250,000 | 191 | 651 |
| 500,000 | 351 | 340 |
| 1,000,000 | 511 — nothing dropped | 0 |
| 100,000,000 (default) | 511 — nothing dropped | 0 |

**What this does *not* do, also measured.** The part of the image above the line is
**pixel-identical** to a clean render of the same scene (0 differing pixels in all three
overflowing cases), the line does **not** flicker — it sat on the same scanline across four
consecutive frames of a static camera — and nothing crashes. Splats are not
mis-sorted or mis-coloured; the ones that survive are exactly right, and the rest are
simply absent.

**You are told when it happens.** The log carries a one-shot warning
(`Overlap-record overflow: the tile-binning pass dropped overlap records …`) whenever any
record is dropped; use it to tell whether you are hitting this at all. The profiler monitor
`gaussian_splatting/overflow_tile_count` counts only tiles that were **emptied completely**.
A tile cut off partway, such as the seam row, loses splats without being counted, so a
small overflow confined to the last tiles can leave the monitor at 0. Both signals fired
in the run above.

**Is it transient or permanent?** That depends on which side of the *configured cap* your
demand is on. When the renderer sees a drop it grows its capacity toward 1.5×
the demand it measured — but it will not grow past `max_overlap_records`, whatever you have set
that to. So:

- **Demand below your `max_overlap_records`, capacity merely behind it** — a spike, and it
  fixes itself as the capacity catches up. This is the case you can meet at defaults.
- **Demand above your `max_overlap_records`** — **permanent**. The renderer cannot grow past
  the setting, and the band stays. Every measurement in the table above is this case: the frame
  was still truncated on the 24th frame, on exactly the same scanline as the first.
- **The renderer cannot allocate a bigger buffer** (VRAM is short) — **lasting while memory is
  short**, even below your `max_overlap_records`. It keeps the old capacity and retries, and
  the log says so (`Global composite sort grow to … could not build its replacement`). Raising
  `max_overlap_records` does not help here and asks for more memory; free VRAM or reduce
  density instead.

At the 100,000,000 default the second case needs a frame demanding more than 100 million
records. The 100,000-splat scene above demands 708,814.

**Workaround:** reduce splat density, or move the camera back, so fewer splats cover each
tile. If you lowered `max_overlap_records`, raise it back — do not set it below your
scene's demand. Raising it above the default costs VRAM as demand grows: each record needs
24 bytes with the default 64-bit keys (key and value, plus an equal-sized radix-sort copy),
so a scene that actually uses 100M records holds about **2.4 GB** in these buffers alone.
Prefer reducing density.

**Not measured:** whether a real scene at default settings can exceed 100,000,000 records.
Scaling the numbers above to 1080p and the node's own 500,000-splats-per-frame cap gives
roughly 28 million — about 3.6× of headroom — but that is arithmetic on a synthetic grid,
not a capture of real content. If you hit the warning above at default settings, that is
worth reporting on #54.

---

**One proposal is outstanding: #54, above.** The section is otherwise empty, and it is kept
even when empty because the mechanism is the point: it is where a disclosure is drafted
while its disposition is still open, and an empty section is a statement that no such draft
is outstanding — not an invitation to skip the step.

A previous entry here was **#1025** (splats trailing under TAA in motion). A named human
accepted it on
[2026-09-20](https://github.com/klausi3D/godotGS/issues/1025#issuecomment-5752837553), which
met the acceptance bar's §10.1 condition 4, so it moved up into
[Rendering](#rendering) as a real limitation. An earlier revision of that entry said splats
ghost under FSR2 too and told you to avoid it; that was a prediction, it was measured, and it
was wrong in the direction that costs you a working feature — which is the reason this
section exists at all.
