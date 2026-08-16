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


_HOST_FIELD_SEQ = re.compile(
    r"float\s+depth_linearize_add\s*;.*?int32_t\s+source_decode_srgb\s*;", re.DOTALL
)
_SHADER_FIELD_SEQ = re.compile(
    r"float\s+depth_linearize_add\s*;.*?int\s+source_decode_srgb\s*;", re.DOTALL
)


def check_push_constant_mirror(failures: list[str]) -> None:
    host = _read(OUTPUT_COMPOSITOR, failures)
    shader = _read(VIEWPORT_BLIT_GLSL, failures)
    if host is None or shader is None:
        return
    if not _HOST_FIELD_SEQ.search(host):
        failures.append(
            f"{_rel(OUTPUT_COMPOSITOR)}: ViewportBlitPushConstant must declare "
            "`int32_t source_decode_srgb;` after `float depth_linearize_add;` (former pad0 slot)"
        )
    if not _SHADER_FIELD_SEQ.search(shader):
        failures.append(
            f"{_rel(VIEWPORT_BLIT_GLSL)}: BlitParams must declare "
            "`int source_decode_srgb;` after `float depth_linearize_add;` (former pad0 slot)"
        )
    if "params.source_decode_srgb != 0" not in shader:
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


def self_test() -> int:
    """Prove the checker discriminates: the clean tree must pass, and a
    synthetic copy with the pre-upscale hook removed must be flagged. Runs on
    temp copies only — never mutates the repository."""
    global FORWARD_CLUSTERED
    baseline = run_checks()
    if baseline:
        print("[self-test] cannot self-test on a failing tree:")
        for line in baseline:
            print(f"  {line}")
        return 1

    # Ordering check must fire when the hook is moved after its consumers.
    fc_text = FORWARD_CLUSTERED.read_text(encoding="utf-8")
    hook_anchor = "gaussian_composite_pre_upscale = true;"
    mutated = fc_text.replace(hook_anchor, "/* hook removed */", 1)
    failures: list[str] = []
    pos = mutated.find(hook_anchor)
    if pos != -1:
        print("[self-test] mutation failed to remove the hook anchor")
        return 1
    # Re-run check A against the mutated text through a temp file swap-in.
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "render_forward_clustered.cpp"
        tmp.write_text(mutated, encoding="utf-8")
        original = FORWARD_CLUSTERED
        try:
            FORWARD_CLUSTERED = tmp
            check_forward_clustered(failures)
        finally:
            FORWARD_CLUSTERED = original
    if not failures:
        print("[self-test] checker did NOT flag a removed pre-upscale hook — vacuous guard")
        return 1
    print("[self-test] OK: clean tree passes; removed hook is flagged.")
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
