#!/usr/bin/env python3
"""Guard the GPU-001 Option B pre-upscale composite contract (refs #921).

Contract (decision memo 2026-08-16, Option B — pre-tonemap/pre-upscale hook):
the Gaussian-splat render+composite must run INSIDE the forward-clustered
`_render_scene`, writing the internal scene color buffer BEFORE every consumer
of that buffer (FSR2 / MetalFX-temporal / TAA / tonemap). The legacy post-scene
hook wrote the internal texture after those consumers, so splats were silently
absent for every scaled/temporal viewport at the shipped default
`composite/depth_test=true` (GS-AUDIT-GPU-001, runtime-confirmed in Phase 0).

This checker asserts the source-level invariants that keep the fix alive:

  A. render_forward_clustered.cpp runs the pre-upscale hook (sets the
     `gaussian_composite_pre_upscale` flag, then renders+commits) and that hook
     precedes the FSR2 upscale, the MetalFX-temporal process, the TAA process,
     and the tonemap call in file order. The consumer list is the full set the
     contract names (memo section 3, Option B); it is an explicit enumeration —
     deriving "every reader of the internal color buffer" would need semantic
     analysis a static text guard cannot do faithfully, so a NEW consumer added
     upstream must be added here in the same change (Codex #924 review thread,
     MetalFX finding).
  B. renderer_scene_render_rd.cpp keeps the legacy post-scene hook gated on
     `!render_data.gaussian_composite_pre_upscale` (mobile/multiview/probe
     fallback stays, but never double-composites).
  C. output_compositor.cpp pins the pre-upscale composite destination to the
     internal buffer (no present redirect), routes the pre-upscale phase away
     from the present-framebuffer graphics blend, and requests the sRGB->linear
     source decode.
  D. render_data_rd.h declares the phase flag.
  E. The `source_decode_srgb` push-constant field exists on BOTH sides of the
     host<->shader mirror, in the same slot (immediately after
     `depth_linearize_add`). The generic push-constant layout guard in
     check_gaussian_layout_sync.py defers viewport_blit's BlitParams (ivec2
     packing), so this positional anchor is the parity check for this field.
  F. The `destination_has_alpha` push-constant field (#928) does the same, in
     the former pad1 slot immediately after `source_decode_srgb`, is READ by the
     shader, and is REQUESTED by the pre-upscale phase. Declaring it without
     reading it, or dropping the request, silently restores the defect: every
     composited texel forced to alpha=1.0, which flattens splat coverage on a
     transparent_bg viewport and is invisible to every opaque-viewport oracle.

  G. The PAINTERLY viewport composite obeys the same phase contract (#986).
     It is a graphics pass that draws into render_buffers->get_internal_texture()
     unconditionally, so it is correct only at the pre-upscale seam and must stay
     gated on the phase; in the legacy post-scene phase the internal buffer has
     already been consumed and the write would be invisible. Its shader must come
     from the EMBEDDED PainterlyCompositeShaderRD, never from a runtime file load:
     the file-load path could not compile (the two on-disk copies began with
     ShaderRD stage delimiters, which are not GLSL) and its `res://modules/...`
     paths do not exist in an exported project at all. And because its destination
     is the LINEAR pre-tonemap buffer, the fragment must apply the same
     sRGB->linear source decode the compute blit applies under
     `source_decode_srgb` (#930) -- on UNPREMULTIPLIED values, with the exact
     piecewise EOTF. Dropping any one of the three is silent: the composite stops
     running, or runs in the wrong colour space, with no warning either way.
     It must also take its scene-depth math from the SHARED guard include and the
     one host derivation, use the shared depth epsilon, and gate the test on
     composite/depth_test -- a private copy of any of those is what discarded
     every painterly fragment while the composite looked like it was running.

Every anchor is fail-closed: a missing file or a missing/reordered anchor is a
FAILURE, never a skip. If a refactor legitimately moves an anchor, update the
anchor here in the same change — do not weaken it to a substring that would
also match the broken ordering.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

FORWARD_CLUSTERED = ROOT / "servers" / "rendering" / "renderer_rd" / "forward_clustered" / "render_forward_clustered.cpp"
SCENE_RENDER_RD = ROOT / "servers" / "rendering" / "renderer_rd" / "renderer_scene_render_rd.cpp"
RENDER_DATA_RD_H = ROOT / "servers" / "rendering" / "renderer_rd" / "storage_rd" / "render_data_rd.h"
OUTPUT_COMPOSITOR = ROOT / "modules" / "gaussian_splatting" / "interfaces" / "output_compositor.cpp"
VIEWPORT_BLIT_GLSL = ROOT / "modules" / "gaussian_splatting" / "shaders" / "viewport_blit.glsl"
PAINTERLY_RENDERER = ROOT / "modules" / "gaussian_splatting" / "interfaces" / "painterly_renderer.cpp"
PAINTERLY_COMPOSITE_GLSL = ROOT / "modules" / "gaussian_splatting" / "shaders" / "painterly_composite.glsl"


def _rel(path: Path) -> str:
    """Repo-relative when possible, absolute otherwise (self-test uses temp copies)."""
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _read(path: Path, failures: list[str]) -> str | None:
    if not path.is_file():
        failures.append(f"missing file: {_rel(path)}")
        return None
    return path.read_text(encoding="utf-8")


def _find(text: str, anchor: str, label: str, path: Path, failures: list[str]) -> int:
    """Position of a required literal anchor; -1 (and a failure) when absent."""
    pos = text.find(anchor)
    if pos < 0:
        failures.append(f"{_rel(path)}: missing anchor [{label}]: {anchor!r}")
    return pos


def check_forward_clustered(failures: list[str]) -> None:
    text = _read(FORWARD_CLUSTERED, failures)
    if text is None:
        return
    pos_flag = _find(text, "gaussian_composite_pre_upscale = true;", "A: phase flag set", FORWARD_CLUSTERED, failures)
    pos_render = _find(text, "render_gaussian_splats_forward(*p_render_data);", "A: pre-upscale render call", FORWARD_CLUSTERED, failures)
    pos_commit = _find(text, "commit_gaussian_splats(*p_render_data);", "A: pre-upscale commit call", FORWARD_CLUSTERED, failures)
    pos_fsr2 = _find(text, "fsr2_effect->upscale(", "A: FSR2 consumer", FORWARD_CLUSTERED, failures)
    pos_mfx = _find(text, "mfx_temporal_effect->process(", "A: MetalFX-temporal consumer", FORWARD_CLUSTERED, failures)
    pos_taa = _find(text, "taa->process(", "A: TAA consumer", FORWARD_CLUSTERED, failures)
    pos_tonemap = _find(text, "_render_buffers_post_process_and_tonemap(p_render_data);", "A: tonemap consumer", FORWARD_CLUSTERED, failures)
    if min(pos_flag, pos_render, pos_commit, pos_fsr2, pos_mfx, pos_taa, pos_tonemap) < 0:
        return
    rel = _rel(FORWARD_CLUSTERED)
    if not (pos_flag < pos_render < pos_commit):
        failures.append(
            f"{rel}: pre-upscale hook must set gaussian_composite_pre_upscale BEFORE "
            "render_gaussian_splats_forward and commit AFTER it "
            f"(flag@{pos_flag}, render@{pos_render}, commit@{pos_commit})"
        )
    for consumer_label, consumer_pos in (("fsr2_effect->upscale", pos_fsr2), ("mfx_temporal_effect->process", pos_mfx), ("taa->process", pos_taa), ("tonemap", pos_tonemap)):
        if not pos_commit < consumer_pos:
            failures.append(
                f"{rel}: Gaussian pre-upscale composite (@{pos_commit}) must precede "
                f"internal-buffer consumer {consumer_label} (@{consumer_pos}) — "
                "compositing after a consumer writes an already-consumed buffer (GPU-001)"
            )


def check_scene_render_rd(failures: list[str]) -> None:
    text = _read(SCENE_RENDER_RD, failures)
    if text is None:
        return
    gate = re.compile(
        r"if\s*\(\s*!render_data\.gaussian_composite_pre_upscale\s*&&\s*"
        r"!render_data\.gaussian_splat_renderers\.is_empty\(\)\s*\)"
    )
    if not gate.search(text):
        failures.append(
            f"{_rel(SCENE_RENDER_RD)}: legacy post-scene Gaussian hook is no longer "
            "gated on !render_data.gaussian_composite_pre_upscale — the forward-clustered "
            "single-view path would composite twice (or the pre-upscale phase flag was removed)"
        )


def check_render_data_rd(failures: list[str]) -> None:
    text = _read(RENDER_DATA_RD_H, failures)
    if text is None:
        return
    _find(text, "bool gaussian_composite_pre_upscale = false;", "D: phase flag declaration", RENDER_DATA_RD_H, failures)


def check_output_compositor(failures: list[str]) -> None:
    text = _read(OUTPUT_COMPOSITOR, failures)
    if text is None:
        return
    _find(
        text,
        "const bool pre_upscale_phase = p_render_data != nullptr && p_render_data->gaussian_composite_pre_upscale;",
        "C: phase derivation",
        OUTPUT_COMPOSITOR,
        failures,
    )
    _find(
        text,
        "if (!pre_upscale_phase && (!composite_target.is_valid() || can_write_directly_to_present))",
        "C: present redirect gated off in pre-upscale phase",
        OUTPUT_COMPOSITOR,
        failures,
    )
    _find(
        text,
        "else if (!pre_upscale_phase && render_target_framebuffer.is_valid() && !depth_test_enabled)",
        "C: present-framebuffer graphics blend gated off in pre-upscale phase",
        OUTPUT_COMPOSITOR,
        failures,
    )
    _find(
        text,
        "params.source_decode_srgb = pre_upscale_phase;",
        "C: sRGB->linear source decode requested for the linear pre-tonemap destination",
        OUTPUT_COMPOSITOR,
        failures,
    )
    _find(
        text,
        "params.destination_has_alpha = pre_upscale_phase;",
        "F: destination-alpha contract requested for the internal scene buffer (#928)",
        OUTPUT_COMPOSITOR,
        failures,
    )


# Adjacency matters, not mere ordering: `source_decode_srgb` must occupy the
# former pad0 slot — IMMEDIATELY after `depth_linearize_add` and IMMEDIATELY
# before `float pad1` — on both sides, or the host and shader read different
# 4-byte words. The check first EXTRACTS the live declaration block (the host
# `struct ViewportBlitPushConstant {...}` / the shader `BlitParams {...}` push
# constant), then STRIPS // line comments, then requires the three declarations
# with nothing but whitespace between them. Matching the whole file with a
# comment-tolerant regex was green-lightable by a commented-out decoy sequence
# while the live field had moved (Codex #924 round 3, finding 1); a bare `.*?`
# was green-lightable by a field moved after pad1 (round 2, finding 4).
_LINE_COMMENT = re.compile(r"//[^\n]*")
_HOST_BLOCK_HEADER = re.compile(r"struct\s+ViewportBlitPushConstant\s*\{")
_SHADER_BLOCK_HEADER = re.compile(r"layout\s*\(\s*push_constant[^)]*\)\s*uniform\s+BlitParams\s*\{")
#
# `destination_has_alpha` (#928) took the former pad1 slot the same way
# `source_decode_srgb` took pad0, so the sequence is now pinned as a THREE-field
# run with no trailing pad. Both fields are pinned by the same adjacency rule
# and for the same reason: a field that drifts by one 4-byte word makes the host
# and the shader read different words, which no runtime assertion would catch.
_HOST_FIELD_SEQ = re.compile(
    r"float\s+depth_linearize_add\s*;\s*int32_t\s+source_decode_srgb\s*;"
    r"\s*int32_t\s+destination_has_alpha\s*;"
)
_SHADER_FIELD_SEQ = re.compile(
    r"float\s+depth_linearize_add\s*;\s*int\s+source_decode_srgb\s*;"
    r"\s*int\s+destination_has_alpha\s*;"
)


def _extract_stripped_block(text: str, header: re.Pattern, label: str, path: Path, failures: list[str]) -> str | None:
    """The comment-stripped body of the first `header ... { body }` block.
    Both mirror blocks are flat (no nested braces), so the body ends at the
    first `}` after the header. Fail-closed when the header or brace is gone."""
    m = header.search(text)
    if not m:
        failures.append(f"{_rel(path)}: missing anchor [{label}]: declaration block header not found")
        return None
    end = text.find("}", m.end())
    if end < 0:
        failures.append(f"{_rel(path)}: [{label}]: unterminated declaration block")
        return None
    return _LINE_COMMENT.sub("", text[m.end():end])


def check_push_constant_mirror(failures: list[str]) -> None:
    host = _read(OUTPUT_COMPOSITOR, failures)
    shader = _read(VIEWPORT_BLIT_GLSL, failures)
    if host is None or shader is None:
        return
    host_block = _extract_stripped_block(host, _HOST_BLOCK_HEADER, "E: host push-constant struct", OUTPUT_COMPOSITOR, failures)
    if host_block is not None and not _HOST_FIELD_SEQ.search(host_block):
        failures.append(
            f"{_rel(OUTPUT_COMPOSITOR)}: ViewportBlitPushConstant must declare "
            "`int32_t source_decode_srgb;` (former pad0 slot) IMMEDIATELY after "
            "`float depth_linearize_add;`, followed IMMEDIATELY by "
            "`int32_t destination_has_alpha;` (former pad1 slot)"
        )
    shader_block = _extract_stripped_block(shader, _SHADER_BLOCK_HEADER, "E: shader push-constant block", VIEWPORT_BLIT_GLSL, failures)
    if shader_block is not None and not _SHADER_FIELD_SEQ.search(shader_block):
        failures.append(
            f"{_rel(VIEWPORT_BLIT_GLSL)}: BlitParams must declare "
            "`int source_decode_srgb;` (former pad0 slot) IMMEDIATELY after "
            "`float depth_linearize_add;`, followed IMMEDIATELY by "
            "`int destination_has_alpha;` (former pad1 slot)"
        )
    shader_code = _LINE_COMMENT.sub("", shader)
    if "params.source_decode_srgb != 0" not in shader_code:
        failures.append(
            f"{_rel(VIEWPORT_BLIT_GLSL)}: shader never reads params.source_decode_srgb — "
            "the sRGB->linear source decode contract is dead in the blit"
        )
    # A declared-but-unread destination_has_alpha would silently restore the
    # #928 defect (every composited texel forced to alpha=1.0) while the mirror
    # adjacency check above still passed.
    if "params.destination_has_alpha != 0" not in shader_code:
        failures.append(
            f"{_rel(VIEWPORT_BLIT_GLSL)}: shader never reads params.destination_has_alpha — "
            "the destination-alpha contract is dead in the blit, so a transparent "
            "destination's splat coverage is flattened to fully opaque (#928)"
        )


# G: the painterly viewport composite (#986). Anchors are literal because the
# three invariants are each one line of intent, and each one is silent when lost.
def check_painterly_composite(failures: list[str]) -> None:
    compositor = _read(OUTPUT_COMPOSITOR, failures)
    renderer = _read(PAINTERLY_RENDERER, failures)
    shader = _read(PAINTERLY_COMPOSITE_GLSL, failures)
    if compositor is None or renderer is None or shader is None:
        return

    _find(
        _LINE_COMMENT.sub("", compositor),
        "if (pre_upscale_phase && p_painterly_active && subsystem_state.painterly_renderer.is_valid())",
        "G: painterly composite gated on the pre-upscale phase",
        OUTPUT_COMPOSITOR,
        failures,
    )

    renderer_code = _LINE_COMMENT.sub("", renderer)
    _find(
        renderer_code,
        "_compile_composite_shader() != OK",
        "G: painterly composite uses the embedded PainterlyCompositeShaderRD",
        PAINTERLY_RENDERER,
        failures,
    )
    # Fail closed on the RETURN of the runtime loader, not just on the absence of
    # the embedded call: a reverted change would re-add a file load next to it.
    for banned, why in (
        ("load_graphics_shader", "the runtime GLSL file loader (#986 blocker A)"),
        ("painterly_composite.vert.glsl", "an on-disk composite shader path"),
        ("painterly_composite.frag.glsl", "an on-disk composite shader path"),
    ):
        if banned in renderer_code:
            failures.append(
                f"{_rel(PAINTERLY_RENDERER)}: reintroduces {why} (`{banned}`). The composite "
                "shader must come from the embedded PainterlyCompositeShaderRD; a runtime "
                "file load cannot compile the ShaderRD stage delimiters and its res:// paths "
                "do not exist in an exported project."
            )

    shader_code = _LINE_COMMENT.sub("", shader)
    # Decode on UNPREMULTIPLIED values, with the exact EOTF, re-premultiplied by
    # the blend-modulated alpha. Anchoring the whole expression is what makes a
    # partial revert (e.g. decoding the premultiplied sample) visible.
    if "painterly_sample.rgb / painterly_sample.a" not in shader_code:
        failures.append(
            f"{_rel(PAINTERLY_COMPOSITE_GLSL)}: composite does not unpremultiply before "
            "decoding — sRGB decode does not commute with alpha premultiplication (#930)"
        )
    if "srgb_to_linear_exact(straight_srgb) * alpha" not in shader_code:
        failures.append(
            f"{_rel(PAINTERLY_COMPOSITE_GLSL)}: composite does not decode its source with "
            "srgb_to_linear_exact and re-premultiply — its destination is the LINEAR "
            "pre-tonemap internal buffer, so the painterly frame lands in the wrong colour "
            "space with no warning (#930)"
        )
    if '#include "includes/gs_srgb.glsl"' not in shader_code:
        failures.append(
            f"{_rel(PAINTERLY_COMPOSITE_GLSL)}: missing `#include \"includes/gs_srgb.glsl\"` — "
            "srgb_to_linear_exact must come from the shared include, not a local copy that "
            "can drift from the compute blit's decode"
        )
    # The scene-depth guard must come from the shared include and must be GATED on
    # composite/depth_test. Both reverts are silent: a local copy drifts from the
    # compute blit (that divergence is what discarded every fragment, #986), and an
    # ungated test ignores a setting the user set.
    if '#include "includes/gs_scene_depth_guard.glsl"' not in shader_code:
        failures.append(
            f"{_rel(PAINTERLY_COMPOSITE_GLSL)}: missing "
            "`#include \"includes/gs_scene_depth_guard.glsl\"` — the scene-depth occlusion "
            "math must be the one shared with viewport_blit.glsl, not a second copy (#986)"
        )
    if "if (params.depth_test_enabled != 0)" not in shader_code:
        failures.append(
            f"{_rel(PAINTERLY_COMPOSITE_GLSL)}: composite does not gate its scene-depth test "
            "on params.depth_test_enabled — it would depth-test unconditionally and ignore "
            "rendering/gaussian_splatting/composite/depth_test, which the compute composite honours"
        )
    if "gs_scene_depth_occludes(" not in shader_code:
        failures.append(
            f"{_rel(PAINTERLY_COMPOSITE_GLSL)}: shader never calls gs_scene_depth_occludes — "
            "the shared guard is declared but dead"
        )

    # Host side of the same two contracts.
    if "push_constant.depth_epsilon = GS_COMPOSITE_DEPTH_EPSILON_VIEW;" not in renderer_code:
        failures.append(
            f"{_rel(PAINTERLY_RENDERER)}: composite push constant does not use the shared "
            "GS_COMPOSITE_DEPTH_EPSILON_VIEW. A private epsilon moves the painterly occlusion "
            "silhouette away from the compute composite's with no other symptom (#986)"
        )
    if "gs_derive_scene_depth_linearize(" not in renderer_code:
        failures.append(
            f"{_rel(PAINTERLY_RENDERER)}: composite push constant is not filled from "
            "gs_derive_scene_depth_linearize — the ONE host derivation shared with the compute "
            "composite. Deriving scene depth any other way is #986's blocker C"
        )
    if "push_constant.depth_test_enabled = p_scene_depth_test_enabled ? 1 : 0;" not in renderer_code:
        failures.append(
            f"{_rel(PAINTERLY_RENDERER)}: composite push constant does not plumb "
            "composite/depth_test through to the shader"
        )


def run_checks() -> list[str]:
    failures: list[str] = []
    check_forward_clustered(failures)
    check_scene_render_rd(failures)
    check_render_data_rd(failures)
    check_output_compositor(failures)
    check_push_constant_mirror(failures)
    check_painterly_composite(failures)
    return failures


def _run_mutated(global_name: str, mutate, check) -> list[str]:
    """Run one check against a temp copy of its file with `mutate` applied.
    Swaps the module-level path global for the duration; never touches the
    repository files."""
    import tempfile

    g = globals()
    original: Path = g[global_name]
    text = original.read_text(encoding="utf-8")
    mutated = mutate(text)
    if mutated == text:
        return [f"(self-test bug: mutation for {global_name} was a no-op)"]
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / original.name
        tmp.write_text(mutated, encoding="utf-8")
        try:
            g[global_name] = tmp
            check(failures)
        finally:
            g[global_name] = original
    return failures


def self_test() -> int:
    """Prove the checker discriminates: the clean tree must pass, and for EVERY
    check class a synthetic mutation reverting its invariant must be flagged.
    Runs on temp copies only — never mutates the repository.

    Scope note: these are literal/positional source anchors. They defend
    against ACCIDENTAL loss of the contract (refactors, reverts, careless
    edits); an adversarial bypass that keeps the anchors while gutting the
    behavior is out of scope for a static guard — human review is the control
    for that, per review policy."""
    baseline = run_checks()
    if baseline:
        print("[self-test] cannot self-test on a failing tree:")
        for line in baseline:
            print(f"  {line}")
        return 1

    render_call = "render_gaussian_splats_forward(*p_render_data);"
    commit_call = "commit_gaussian_splats(*p_render_data);"
    mutations = (
        # A1: pre-upscale hook flag removed entirely.
        ("A: hook flag removed", "FORWARD_CLUSTERED",
                lambda t: t.replace("gaussian_composite_pre_upscale = true;", "/* hook removed */", 1),
                check_forward_clustered),
        # A2: hook moved AFTER the internal-buffer consumers (render+commit
        # relocated to end of file) — the ordering assertions must fire.
        ("A: hook after consumers", "FORWARD_CLUSTERED",
                lambda t: t.replace(render_call, "", 1).replace(commit_call, "", 1)
                        + "\n\t" + render_call + "\n\t" + commit_call + "\n",
                check_forward_clustered),
        # A3: MetalFX-temporal consumer anchor gone — the guard must fail
        # CLOSED (missing anchor is a failure, never a silently shrunk consumer
        # list). Structurally this also proves the anchor participates in the
        # same ordering loop as the proven FSR2/TAA/tonemap terms.
        ("A3: MetalFX consumer anchor removed", "FORWARD_CLUSTERED",
                lambda t: t.replace("mfx_temporal_effect->process(", "mfx_temporal_effect_renamed(", 1),
                check_forward_clustered),
        # B: legacy post-scene hook loses its phase gate (double composite).
        ("B: legacy gate dropped", "SCENE_RENDER_RD",
                lambda t: t.replace("!render_data.gaussian_composite_pre_upscale && ", "", 1),
                check_scene_render_rd),
        # C: pre-upscale stops requesting the source decode.
        ("C: decode request dropped", "OUTPUT_COMPOSITOR",
                lambda t: t.replace("params.source_decode_srgb = pre_upscale_phase;",
                        "params.source_decode_srgb = false;", 1),
                check_output_compositor),
        # D: phase-flag declaration removed from RenderDataRD.
        ("D: flag declaration removed", "RENDER_DATA_RD_H",
                lambda t: t.replace("bool gaussian_composite_pre_upscale = false;", "", 1),
                check_render_data_rd),
        # E1: shader side of the push-constant mirror reverted to the old pad.
        ("E1: shader mirror reverted", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("int source_decode_srgb;", "float pad0;", 1),
                check_push_constant_mirror),
        # E2: shader field still declared and still read, but MOVED past the
        # trailing slot (host/shader now read different 4-byte words) — the
        # adjacency requirement, not mere ordering, must flag this.
        ("E2: shader field moved past the trailing slot", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("int source_decode_srgb;", "float pad0;", 1)
                        .replace("int destination_has_alpha;",
                                "int destination_has_alpha;\n    int source_decode_srgb;", 1),
                check_push_constant_mirror),
        # E3: same reorder on the HOST side of the mirror.
        ("E3: host field moved past the trailing slot", "OUTPUT_COMPOSITOR",
                lambda t: t.replace("int32_t source_decode_srgb;", "float pad0;", 1)
                        .replace("int32_t destination_has_alpha;",
                                "int32_t destination_has_alpha;\n        int32_t source_decode_srgb;", 1),
                check_push_constant_mirror),
        # E4: field moved after pad1 AND a commented-out decoy of the correct
        # sequence left inside the block — comment stripping must see through
        # the decoy (a whole-file comment-tolerant regex did not).
        ("E4: shader reorder with commented decoy", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("int source_decode_srgb;", "float pad0;", 1)
                        .replace("int destination_has_alpha;",
                                "int destination_has_alpha;\n    int source_decode_srgb;\n"
                                "    // float depth_linearize_add; int source_decode_srgb;"
                                " int destination_has_alpha;", 1),
                check_push_constant_mirror),
        # F1: shader side of the #928 destination-alpha field reverted to the
        # old pad — the mirror adjacency run must fail.
        ("F1: shader destination_has_alpha reverted to pad", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("int destination_has_alpha;", "float pad1;", 1),
                check_push_constant_mirror),
        # F2: same revert on the HOST side of the mirror.
        ("F2: host destination_has_alpha reverted to pad", "OUTPUT_COMPOSITOR",
                lambda t: t.replace("int32_t destination_has_alpha;", "float pad1;", 1),
                check_push_constant_mirror),
        # F3: field still declared on both sides and correctly placed, but the
        # shader stops READING it — this is the #928 defect restored (every
        # composited texel forced opaque) with the adjacency check still green.
        ("F3: shader stops reading destination_has_alpha", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("params.destination_has_alpha != 0", "false", 1),
                check_push_constant_mirror),
        # F4: pre-upscale stops REQUESTING the destination-alpha contract, so
        # the transparent-destination composite silently reverts to opaque.
        ("F4: destination-alpha request dropped", "OUTPUT_COMPOSITOR",
                lambda t: t.replace("params.destination_has_alpha = pre_upscale_phase;",
                        "params.destination_has_alpha = false;", 1),
                check_output_compositor),
        # G1: painterly composite loses its phase gate, so the legacy post-scene
        # phase would draw into an already-consumed internal buffer (#986).
        ("G1: painterly composite phase gate dropped", "OUTPUT_COMPOSITOR",
                lambda t: t.replace(
                        "if (pre_upscale_phase && p_painterly_active && subsystem_state.painterly_renderer.is_valid())",
                        "if (p_painterly_active && subsystem_state.painterly_renderer.is_valid())", 1),
                check_painterly_composite),
        # G2: the embedded ShaderRD call is replaced by the runtime file loader
        # that could never compile — #986 blocker A restored verbatim.
        ("G2: runtime GLSL file loader reintroduced", "PAINTERLY_RENDERER",
                lambda t: t.replace("_compile_composite_shader() != OK",
                        "!p_renderer->load_graphics_shader("
                        "Vector<String>(), Vector<String>()).is_valid()", 1),
                check_painterly_composite),
        # G3: the composite decodes, but on the PREMULTIPLIED sample — the
        # non-commuting case, which looks plausible and is wrong everywhere
        # alpha < 1 (up to ~88% error; see the #930 colour round-trip proof).
        ("G3: decode applied to premultiplied values", "PAINTERLY_COMPOSITE_GLSL",
                lambda t: t.replace("vec3 straight_srgb = painterly_sample.rgb / painterly_sample.a;",
                        "vec3 straight_srgb = painterly_sample.rgb;", 1),
                check_painterly_composite),
        # G4: decode dropped entirely — the pre-fix expression, which writes
        # sRGB-encoded values into the linear pre-tonemap buffer (#930).
        ("G4: source decode dropped", "PAINTERLY_COMPOSITE_GLSL",
                lambda t: t.replace("srgb_to_linear_exact(straight_srgb) * alpha",
                        "straight_srgb * alpha", 1),
                check_painterly_composite),
        # G5: the shared include is replaced by a local copy, which is how the
        # painterly decode would silently drift from the compute blit's.
        ("G5: shared sRGB include replaced by a local copy", "PAINTERLY_COMPOSITE_GLSL",
                lambda t: t.replace('#include "includes/gs_srgb.glsl"',
                        "vec3 srgb_to_linear_exact(vec3 c) { return c * c; }", 1),
                check_painterly_composite),
        # G6: the scene-depth guard reverts to a second local copy — the exact
        # shape that diverged from the compute blit and discarded every fragment.
        ("G6: shared scene-depth guard include dropped", "PAINTERLY_COMPOSITE_GLSL",
                lambda t: t.replace('#include "includes/gs_scene_depth_guard.glsl"', "", 1),
                check_painterly_composite),
        # G7: the depth test stops being gated, so composite/depth_test=false is
        # silently ignored on the painterly path only.
        ("G7: depth-test gate dropped", "PAINTERLY_COMPOSITE_GLSL",
                lambda t: t.replace("if (params.depth_test_enabled != 0)", "if (true)", 1),
                check_painterly_composite),
        # G8: the shared guard is included but never called — declared-and-dead,
        # which no include check can see.
        ("G8: shared guard included but never called", "PAINTERLY_COMPOSITE_GLSL",
                lambda t: t.replace("gs_scene_depth_occludes(", "false && occludes_disabled(", 1),
                check_painterly_composite),
        # G9: host reverts to the private 0.0005 epsilon, moving the painterly
        # silhouette away from the compute composite's with no other symptom.
        ("G9: shared depth epsilon replaced by a private literal", "PAINTERLY_RENDERER",
                lambda t: t.replace("push_constant.depth_epsilon = GS_COMPOSITE_DEPTH_EPSILON_VIEW;",
                        "push_constant.depth_epsilon = 0.0005f;", 1),
                check_painterly_composite),
        # G10: host stops using the ONE scene-depth derivation — blocker C.
        ("G10: host drops the shared scene-depth derivation", "PAINTERLY_RENDERER",
                lambda t: t.replace("gs_derive_scene_depth_linearize(", "legacy_projection_columns(", 1),
                check_painterly_composite),
        # G11: composite/depth_test stops reaching the shader.
        ("G11: depth-test setting not plumbed to the shader", "PAINTERLY_RENDERER",
                lambda t: t.replace("push_constant.depth_test_enabled = p_scene_depth_test_enabled ? 1 : 0;",
                        "push_constant.depth_test_enabled = 1;", 1),
                check_painterly_composite),
    )

    ok = True
    for label, global_name, mutate, check in mutations:
        failures = _run_mutated(global_name, mutate, check)
        if not failures:
            print(f"[self-test] VACUOUS: mutation [{label}] was NOT flagged")
            ok = False
    if not ok:
        return 1
    print(f"[self-test] OK: clean tree passes; all {len(mutations)} reverting mutations are flagged.")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    failures = run_checks()
    if failures:
        print("GS pre-upscale composite hook guard FAILED:")
        for line in failures:
            print(f"  - {line}")
        return 1
    print(
        "GS pre-upscale composite hook guard passed (ordering, phase gating, encoding mirror, "
        "painterly composite reachability + source decode)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
