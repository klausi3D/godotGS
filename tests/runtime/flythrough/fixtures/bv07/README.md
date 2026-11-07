# BV-07 probe-lane test fixtures (captured)

These files are real producer output, captured once and checked in so the guard lane can
test the probe lane's parser and summary without a GPU (`tests/AGENTS.md`: never
hand-author a fixture that claims to model a producer's output). Re-capture them, do not
edit them, when the trace format, the driver or the path changes.

| File | Producer | How it was captured |
| --- | --- | --- |
| `tsmoke.manifest.json` | BV-05 generator | `python tests/runtime/flythrough/generate_world.py --godot-binary <opt build> --tier T-smoke --out <dir>`; the sidecar manifest, verbatim |
| `tsmoke_path.trace.bin.gz`, `tsmoke_path.result.json` | `probe_world.gd` (path mode) | `python tests/runtime/flythrough/run_probe.py --godot-binary <opt build> --world T-smoke=<dir> --out <out> --no-sweep --passes 1 --max-fps 15`; `<out>/T-smoke/path_run/trace.bin` gzipped, `result.json` verbatim. The 15 fps cap only keeps the fixture small; measurement runs are uncapped. |
| `tsmoke_sweep.trace.bin.gz`, `tsmoke_sweep.result.json` | `probe_world.gd` (sweep mode) | `run_probe.run_godot` on `run_probe.sweep_plan(<T-smoke world>, run_probe.sweep_views(<manifest>), 0, 0.5)` with `max_fps = 30`; trace gzipped, result verbatim. |

All three were captured on Windows 11 with an RTX 3090 and a
`target=editor dev_build=no optimize=speed_trace tests=yes` build of the BV-07 branch.
The result files carry the absolute paths of that run; nothing reads them.
