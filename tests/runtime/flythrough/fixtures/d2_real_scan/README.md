# D2 real-scan identity test fixture (captured)

Verbatim producer output from workflow run
[43434714271](https://github.com/klausi3D/godotGS/actions/runs/43434714271), artifact
`d2-real-scan-identity-43434714271-1`, on `gpu-linux-3080`. The tests-enabled binary
was built from head `5c773ab8d76`. The case used `bicycle-30k.ply`, keeping
zero-based vertex i iff i % 7 == 0 (stride 7, 875,994 of 6,131,954 splats).

These files were captured, not authored (`tests/AGENTS.md`); re-capture them rather
than editing them. They hold no splat payload or scan bytes, only digests, counts
and metadata, including the doctest execution summary.

| File | Producer | SHA-256 |
| --- | --- | --- |
| `real_scan.doctest.json` | D2 real-scan C++ doctest report | `4999170a82af41624b53c52df85db4dc9828a9ce8411c008e26880ee07a4d0ce` |
| `real_scan.doctest_stdout.txt` | stdout of the same exact-name doctest run | `4db9ebda9d275d025a69b46aa43375396828946d68038de69986b10b7a7825c6` |

`test_dispatch_lane.py` copies the captured report to the runner's report path and
uses the captured stdout. Failure and admission cases mutate deep copies of this
report; the captured files remain unchanged.
