# BV-09 gate-verdict test fixture (captured)

The output of the first valid M1 run, tier A on the RTX 3080 runner: workflow run
[42512864344](https://github.com/klausi3D/godotGS/actions/runs/42512864344), artifact
`beyond-vram-dispatch-A-42512864344-1`, binary `editor_tests` sha256 `3061bdd5…` built from
`46e1c556aa0f5980123e22cde6b419df5c9af5d6`. Status OK, 16 of 16 checkpoints settled with the
chunk cap at 480, regulator 0 adjustments. It is real producer output (`tests/AGENTS.md`: never
hand-author a fixture that claims to model a producer's output); re-capture it, do not edit it.

| File | Producer | How it was captured |
| --- | --- | --- |
| `summary.json.gz` | BV-07 `run_probe.py` | the artifact's `probe/summary.json`, byte for byte (sha256 `5b603eb6…6282`), gzipped with mtime 0 |
| `build_info.json` | `dispatch_lane.py build-info` | verbatim |
| `steps/*.json` | `dispatch_lane.py` step records | verbatim; the run predates BV-06b, so there is no `contention` or `cache_check` record |

`tests/runtime/flythrough/test_flythrough_verdict.py` reads these files, mutates one signal per
case and asserts the verdict. The world digests, settings and device in
`flythrough_thresholds.json` were copied from the same run (see its `provenance`).
