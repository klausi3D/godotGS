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
     precedes the FSR2 upscale, the TAA process, and the tonemap call in file
     order.
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
    pos_taa = _find(text, "taa->process(", "A: TAA consumer", FORWARD_CLUSTERED, failures)
    pos_tonemap = _find(text, "_render_buffers_post_process_and_tonemap(p_render_data);", "A: tonemap consumer", FORWARD_CLUSTERED, failures)
    if min(pos_flag, pos_render, pos_commit, pos_fsr2, pos_taa, pos_tonemap) < 0:
        return
    rel = _rel(FORWARD_CLUSTERED)
    if not (pos_flag < pos_render < pos_commit):
        failures.append(
            f"{rel}: pre-upscale hook must set gaussian_composite_pre_upscale BEFORE "
            "render_gaussian_splats_forward and commit AFTER it "
            f"(flag@{pos_flag}, render@{pos_render}, commit@{pos_commit})"
        )
    for consumer_label, consumer_pos in (("fsr2_effect->upscale", pos_fsr2), ("taa->process", pos_taa), ("tonemap", pos_tonemap)):
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
_HOST_FIELD_SEQ = re.compile(
    r"float\s+depth_linearize_add\s*;\s*int32_t\s+source_decode_srgb\s*;\s*float\s+pad1\s*;"
)
_SHADER_FIELD_SEQ = re.compile(
    r"float\s+depth_linearize_add\s*;\s*int\s+source_decode_srgb\s*;\s*float\s+pad1\s*;"
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
            "`int32_t source_decode_srgb;` IMMEDIATELY between `float depth_linearize_add;` and `float pad1;` (the former pad0 slot)"
        )
    shader_block = _extract_stripped_block(shader, _SHADER_BLOCK_HEADER, "E: shader push-constant block", VIEWPORT_BLIT_GLSL, failures)
    if shader_block is not None and not _SHADER_FIELD_SEQ.search(shader_block):
        failures.append(
            f"{_rel(VIEWPORT_BLIT_GLSL)}: BlitParams must declare "
            "`int source_decode_srgb;` IMMEDIATELY between `float depth_linearize_add;` and `float pad1;` (the former pad0 slot)"
        )
    if "params.source_decode_srgb != 0" not in _LINE_COMMENT.sub("", shader):
        failures.append(
            f"{_rel(VIEWPORT_BLIT_GLSL)}: shader never reads params.source_decode_srgb — "
            "the sRGB->linear source decode contract is dead in the blit"
        )


def run_checks() -> list[str]:
    failures: list[str] = []
    check_forward_clustered(failures)
    check_scene_render_rd(failures)
    check_render_data_rd(failures)
    check_output_compositor(failures)
    check_push_constant_mirror(failures)
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
        # E2: shader field still declared and still read, but MOVED after pad1
        # (host/shader now read different 4-byte words) — the adjacency
        # requirement, not mere ordering, must flag this.
        ("E2: shader field moved after pad1", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("int source_decode_srgb;", "float pad0;", 1)
                        .replace("float pad1;", "float pad1;\n    int source_decode_srgb;", 1),
                check_push_constant_mirror),
        # E3: same reorder on the HOST side of the mirror.
        ("E3: host field moved after pad1", "OUTPUT_COMPOSITOR",
                lambda t: t.replace("int32_t source_decode_srgb;", "float pad0;", 1)
                        .replace("float pad1;", "float pad1;\n        int32_t source_decode_srgb;", 1),
                check_push_constant_mirror),
        # E4: field moved after pad1 AND a commented-out decoy of the correct
        # sequence left inside the block — comment stripping must see through
        # the decoy (a whole-file comment-tolerant regex did not).
        ("E4: shader reorder with commented decoy", "VIEWPORT_BLIT_GLSL",
                lambda t: t.replace("int source_decode_srgb;", "float pad0;", 1)
                        .replace("float pad1;",
                                "float pad1;\n    int source_decode_srgb;\n"
                                "    // float depth_linearize_add; int source_decode_srgb; float pad1;", 1),
                check_push_constant_mirror),
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
    print("GS pre-upscale composite hook guard passed (ordering, phase gating, encoding mirror).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
