# Benchmark Runner

!!! note "Scope"
    This page covers the **runtime benchmark lanes** driven by `tests/runtime/run_benchmark.py`. For the `--gs-gpu-test` doctest harness (`tests/ci/run_gpu_harness.py`, `CompositorHazard` regression), see the [Testing Setup Guide](setup-guide.md#gpu-test-harness-gs-gpu-test). The two lanes do not overlap.

## Canonical Command

Use this as the single benchmark entrypoint. Build the binary **optimized** first — a
`dev_build=yes` binary is compiled at `-O0` and its numbers are not performance evidence:

```bash
# Optimized build. Note there is no `.dev.` in the resulting binary name.
scons platform=<platform> target=editor dev_build=no optimize=speed debug_symbols=no tests=yes

python3 tests/runtime/run_benchmark.py \
  --godot-binary ./bin/godot.linuxbsd.editor.x86_64 \
  --project-path ./tests/examples/godot/test_project \
  --profile performance
```

!!! danger "Do not benchmark a `.dev.` binary"
    `dev_build=yes` produces `godot.<platform>.editor.dev.<arch>` — the `.dev.` infix is the
    tell. Passing such a binary to `--godot-binary` produces numbers inflated by roughly an order
    of magnitude on the CPU side, and it is how the meaningless row published here before
    2026-07-19 was produced. The optimized binary has no `.dev.` infix
    (`godot.linuxbsd.editor.x86_64`, `godot.windows.editor.x86_64.exe`). Check the filename you
    pass before trusting any result.

For ad hoc local exploration, `--profile` still defaults to `everything`. For benchmark evidence
and Tier 2 closeout, `--profile performance` is the canonical lane set.

## Current Public Snapshot

The current committed public result is a five-lane set captured on 2026-07-19 at commit
`9161d92f349` on an **optimized** Windows build (`dev_build=no optimize=speed`), RTX 3090 /
Ryzen 7 5800X. Values are steady-state, run 2 of 3.

**`9161d92f349` was 8 commits behind `master` (`ab847aeabf3`) when this was published**, three of
them touching runtime paths (#665, #666, #667). The
[Performance Dashboard](../performance/index.md#currency) records why those are not expected to
move these numbers. Re-measure rather than trusting that reasoning if a change lands on the
per-frame render path.

| Lane | Score | Avg FPS | P99 Frame (ms) | GPU Time (ms) |
| --- | ---: | ---: | ---: | ---: |
| `dense_resident_2m` **(published baseline)** | 24.0 | **12.3** | 87.47 | 51.93 |
| `static_baseline` | 97.2 | 455.1 | 3.05 | 1.69 |
| `city_flyover` | 99.3 | 128.7 | 8.33 | 6.83 |
| `lighting_stress` | 90.4 | 72.6 | 15.15 | 13.90 |
| `instance_storm` | 49.3 | 31.5 | 31.82 | 30.09 |

The headline figure is `dense_resident_2m`'s ~12.3 FPS. `static_baseline`'s 455.1 FPS is a
low-noise regression reference on a single 10,000-splat instance, not a capability claim; it held
`evidence_role: published_baseline` until [#790](https://github.com/klausi3D/godotGS/issues/790)
and no longer does.

This snapshot was captured before that change, when `dense_resident_2m` had `weight: 0.0` and was
a member of no default profile — so the committed scores exclude it from the aggregate. The lane
is now weighted in the `performance` profile; the next capture will report a lower aggregate for
that reason. No measured value above was re-run or altered.

That snapshot is what backs the public performance dashboard. Full hardware context, per-pass GPU
breakdown, variance, and caveats live on the [Performance Dashboard](../performance/index.md).

!!! warning "Only publish optimized-build numbers"
    A `dev_build=yes` binary is compiled at `-O0` and inflates CPU-side frame cost by roughly an
    order of magnitude. The row published here before 2026-07-19 came from a
    `godot.linuxbsd.editor.dev.x86_64` binary and was not valid performance evidence. Record the
    exact build flags with any snapshot you add.

## Standard Flags

- `--profile` (`everything|quick|performance|synthetic-only|ab-only`)
- `--godot-binary`
- `--project-path`
- `--output-dir`
- `--reference-dir`
- `--capture`
- `--fail-fast`

Compatibility extras are still accepted (`--capture-lane`, `--no-captures`, `--no-dashboard`) but the command above is the supported path.

## Runner Execution

`tests/runtime/run_benchmark.py` is the canonical entry point.

It launches one Godot subprocess per benchmark lane, writes per-lane JSON and log files into the selected output directory, then aggregates those lane results into the suite report and summary.

## Profiles

- `everything`: suite lanes + unified + small baseline + synthetic scenes
- `quick`: shortened smoke profile
- `performance`: canonical benchmark evidence profile
- `synthetic-only`: synthetic scenes only
- `ab-only`: instance pipeline serial vs single-pass lanes

## Asset Policy

Asset generation and mapping are canonicalized through:

- `tests/fixtures/benchmark_asset_manifest.json`

The benchmark runner resolves assets through the project-local benchmark manifest by default.
That manifest now carries both:

- the concrete asset path resolution order
- the declared asset/evidence classification for each lane

Use the classifications literally:

- `real_chunked`: representative chunked-scene benchmark evidence
- `deterministic_synthetic`: synthetic support lane
- `lightweight_smoke`: lightweight smoke/support lane, not representative large-scene evidence

Run synthetic asset preparation before benchmark collection whenever fixture or manifest state may
be stale:

```bash
python3 tests/runtime/prepare_synthetic_assets.py --quiet \
  --godot-binary ./bin/<your-godot-binary>
```

!!! important "Pass `--godot-binary` for benchmark collection"
    Several fixtures — including `test_splats.ply`, which most lanes resolve to — are listed in
    `CPP_GENERATED_FILENAMES` and are produced by the engine's `[GeneratePLY]` test case when a
    binary is supplied. Without `--godot-binary` the script falls back to lightweight Python
    generators and writes `test_splats.ply` with **1024** splats instead of **10000**. Both forms
    are valid for smoke coverage, but they are different workloads and will not reproduce
    published benchmark numbers. Since #669 the benchmark harness enforces this rather than
    trusting it: a lane loading the 1024-splat fixture fails instead of reporting a number.

`test_splats.ply` is gitignored and never committed, so it is absent from a fresh clone.

Since #669 a lane **fails closed** on a fixture that is absent, unreadable, or smaller than the
lane declares it needs:

* `run_benchmark.py` refuses to start the suite (exit `2`) and names the asset, the resolved
  on-disk path, and the prep command.
* A directly-launched lane scene aborts with exit `3`, writes **no** report, and prints
  `[BENCH-LANE] FATAL` lines naming the asset and the prep command.

The required counts are declared in `asset_min_splat_counts` in the benchmark asset manifest and
are generated from `ASSET_MIN_SPLAT_COUNTS` in `prepare_synthetic_assets.py`. This is what makes
the `--godot-binary` requirement above enforced rather than merely documented: a 1024-splat
Python-fallback `test_splats.ply` fails the 10000-splat floor instead of silently benchmarking a
10x-smaller workload.

### Which producer wrote the fixture (#790)

A floor answers "is it big enough". It cannot answer "which corpus is this", and for the committed
`synthetic_*.ply` fixtures it never could: their floors are set equal to the committed
Python-fallback sizes so that a clean checkout passes, so a 2048-splat sphere and a 50000-splat
sphere both satisfy `synthetic_sphere.ply`. Raising those floors is not available — it would fail
every fresh clone.

So the manifest also carries `asset_expected_splat_counts`: the **exact** count each producer
writes, per fixture. Both numbers are derived from their producers — `CANONICAL_SPECS` for the
Python fallback, `generate_synthetic_ply_fixtures.h` for the C++ generators — so neither is a
number anyone chose.

| | effect |
| --- | --- |
| Count matches a declared producer | the lane is labelled `python_fallback` or `cpp_rich` |
| Count matches **no** declared producer | preflight fails `UNRECOGNIZED` — a thinned, truncated or hand-edited fixture cannot be attributed to a workload |
| Asset declares no producers (`--generate-dummy-assets`, chunked ladder) | `undeclared`, no size rule |

`run_benchmark.py` prints the label per lane before measuring anything, writes it into each lane's
result (`asset_variant`, `asset_splat_count`) and into a **Fixture Provenance** section of
`benchmark_suite_summary.md`, so a stored number can still be attributed to a workload later.
`--require-asset-variant cpp_rich` turns the label into enforcement; lanes whose asset has no C++
generator (`synthetic_spiral`, `synthetic_flower_field`) are exempt, because demanding a producer
that does not exist is a gate that can never go green.

!!! warning "Only the benchmark evidence job preps with `--godot-binary`"
    `tests/ci/run_module_tests.py`, `tests/ci/run_baseline_qa.py` and
    `tests/runtime/run_runtime_validation.py` deliberately do **not**. They share a workspace with
    the QA scene suite, whose committed expectations were measured against the Python-fallback
    fixture: `tests/ci/baselines/qa_results.json` records `source_splat_count: 1024`, and the
    committed `test_splats.gsplatworld` is a 1024-splat bake that the world-vs-instance A/B
    compares against the PLY. Regenerating at the C++ count there would break a blocking gate
    rather than improve a benchmark. Moving that corpus is a baseline change — rebake the world,
    re-measure the QA baseline, and forward the binary in one sequence.
    `tests/ci/test_benchmark_fixture_contract.py::FallbackPinnedCorpusTests` keeps the coupling
    enforced, and each runner states it at the point of prep.

Before #669 such a lane instantiated zero splat nodes and reported an *implausibly high* FPS with
a passing recommendation — the failure presented as a spectacular result rather than a broken one.
Regression coverage lives in `tests/ci/test_benchmark_fixture_contract.py`.

Streaming-named lanes that still resolve to `test_splats.ply` are intentionally classified as
`lightweight_smoke`; they are useful for proof-shape smoke coverage, but they are not chunked
large-scene evidence.

## CI Surfaces

`streaming-gpu-ci` is the only blocking GPU-backed streaming gate. The benchmark proof
surfaces below are evidence-only and should not be treated as a second streaming gate.

| Surface | Runner command | Scope | Blocking? |
| --- | --- | --- | --- |
| `streaming-gpu-ci` | `python3 tests/runtime/run_runtime_validation.py --profile streaming-gpu-ci` | Runtime validation for residency and world-streaming regressions | Yes |
| `openworld-proof-dev` | `python3 tests/runtime/run_benchmark.py --profile performance --lane open_world_corridor_proof --lane city_flyover` | `20M corridor` candidate + boundary-crossing smoke support | No |
| `openworld-proof-weekly` | `python3 tests/runtime/run_benchmark.py --profile performance --lane long_soak` | City-roam soak smoke support | No |

## Proof Ladder

The current large-world proof ladder is intentionally split between:

- one dedicated world-consuming bootstrap benchmark lane
- the staged chunked asset contract for the wider ladder
- the existing runtime probes

No shared `GaussianSplatAsset` benchmark lane should reference the chunked ladder. The only
committed runnable benchmark surface is the dedicated `open_world_corridor_proof` lane, which
consumes the staged corridor contract and builds a local `GaussianSplatWorld` runtime surface
at the declared 20M total-splat scale.

| Proof role | Surface | Failure intent | Current evidence class |
| --- | --- | --- | --- |
| `proof_corridor_return_bootstrap` | `open_world_corridor_proof` | Fail when the dedicated world-consuming 20M corridor surface cannot consume the staged contract and produce a runnable large-world chunked path. | `chunked_open_world_candidate` |
| `proof_support_corridor_churn_smoke` | `streaming_corridor` | Catch corridor-shaped churn regressions before the real chunked corridor lane is promoted. | `lightweight_smoke` |
| `proof_support_boundary_crossing_smoke` | `city_flyover` | Catch large visibility-shift / boundary-crossing regressions on the existing smoke surface. | `lightweight_smoke` |
| `proof_support_city_roam_soak_smoke` | `long_soak` | Catch revisit / soak regressions on the current smoke surface before the city-scale chunked lane is validated. | `lightweight_smoke` |
| `runtime_budget_churn_probe` | `tests/runtime/test_gpu_streaming_eviction_churn_probe.gd` | Fail on budget-driven churn collapse, queue starvation, or stalled forward progress under tight VRAM. | Runtime probe |
| `runtime_corridor_return_gate` | `tests/runtime/test_world_streaming_gate.gd` | Fail when forward and return phases never reach real streamed readiness under the streaming route. | Runtime gate |
| `runtime_large_tier_stress` | `tests/runtime/test_gpu_streaming_stress.gd` | Fail when large-tier streaming loses first-visible, residency, or frame-budget stability. | Runtime stress |

Mixed-residency and multi-asset hub scenarios remain evidence-only and are intentionally outside
the blocking proof ladder for `Phase 4C.1`.

The benchmark lanes above compose the evidence surfaces as follows:

- `openworld-proof-dev` = `open_world_corridor_proof` + `city_flyover`
- `openworld-proof-weekly` = `long_soak`
- only `open_world_corridor_proof` is a large-world candidate lane today; `city_flyover` and `long_soak` remain smoke-support surfaces until the `50M boundary` and `100M city` lanes are runnable
- both surfaces are benchmark evidence only, while `streaming-gpu-ci` remains the only blocking gate

## Large-World Proof Contract

The benchmark proof contract is emitted per lane in runner JSON via:

- `proof_contract`
- `proof_metrics`
- `proof_status`
- `proof_failures`
- `proof_warnings`
- `proof_missing_telemetry`

Status meaning is intentionally narrow:

- `pass`: lane met correctness and soft-budget thresholds
- `warn`: correctness contract passed, but soft-budget thresholds exceeded
- `fail`: correctness / streaming-behavior contract failed
- `missing_telemetry`: runner could not audit one or more required proof metrics

The contract is currently locked for these ladder lanes:

| Lane | Proof role | Correctness / streaming behavior thresholds | Soft budget warnings |
| --- | --- | --- | --- |
| `open_world_corridor_proof` | Corridor return | `first_visible_ms <= 3500`, `residency_ratio >= 0.70`, `queue_pressure_frames <= 32`, `no_progress_frames <= 6`, `scan_starved_frames <= 6`, `vram_cap_hit_frames <= 0` | `frame_p95_ms <= 95`, `frame_p95_to_avg_ratio <= 2.25`, `chunk_loads_per_frame.p95 <= 10`, `chunk_evictions_per_frame.p95 <= 4` |
| `city_flyover` | Boundary crossing | `first_visible_ms <= 4500`, `residency_ratio >= 0.65`, `queue_pressure_frames <= 48`, `no_progress_frames <= 12`, `scan_starved_frames <= 12`, `vram_cap_hit_frames <= 4` | `frame_p95_ms <= 120`, `frame_p95_to_avg_ratio <= 2.50`, `chunk_loads_per_frame.p95 <= 12`, `chunk_evictions_per_frame.p95 <= 8` |
| `long_soak` | City roam + soak | `first_visible_ms <= 6000`, `residency_ratio >= 0.75`, `queue_pressure_frames <= 96`, `no_progress_frames <= 24`, `scan_starved_frames <= 20`, `vram_cap_hit_frames <= 8` | `frame_p95_ms <= 135`, `frame_p95_to_avg_ratio <= 2.60`, `chunk_loads_per_frame.p95 <= 8`, `chunk_evictions_per_frame.p95 <= 10` |

Metric intent:

- correctness thresholds are meant to capture lost visibility, stalled forward progress, or residency collapse
- soft budget warnings are meant to flag machine-noise-sensitive frame spikes or bursty load/eviction pressure without turning one noisy run into a hard blocker
- missing telemetry is a separate review condition because an unauditable lane is not valid proof evidence

## Suite Coverage

These are the user-relevant lanes already encoded in the suite and available for publication once committed results exist:

| Lane | Purpose | Asset class | Evidence role | Current publication status |
| --- | --- | --- | --- | --- |
| `dense_resident_2m` | Dense resident path, ~4.9M visible splats (196 x `synthetic_spiral`) | `deterministic_synthetic` | `published_baseline` | Published |
| `static_baseline` | Low-noise regression reference (10k splats) | `lightweight_smoke` | `low_noise_smoke_reference` | Published |
| `open_world_corridor_proof` | Dedicated world-consuming 20M corridor proof | `chunked_open_world_candidate` | `proof_corridor_return_bootstrap` | Suite-only |
| `streaming_corridor` | Corridor churn smoke support | `lightweight_smoke` | `proof_support_corridor_churn_smoke` | Suite-only |
| `city_flyover` | Boundary-crossing smoke support | `lightweight_smoke` | `proof_support_boundary_crossing_smoke` | Suite-only |
| `instance_storm` | Many-instance submission pressure | `lightweight_smoke` | Suite support | Suite-only |
| `lighting_stress` | Animated light and shading stress | `lightweight_smoke` | Suite support | Suite-only |
| `long_soak` | City-roam soak smoke support | `lightweight_smoke` | `proof_support_city_roam_soak_smoke` | Suite-only |
| `unified_composite` | Integrated composite smoke support | `lightweight_smoke` | `proof_support_integrated_composite_smoke` | Suite-only |

No currently documented benchmark lane should be cited as representative chunked streaming
evidence unless its manifest classification is upgraded to `real_chunked`. The dedicated
`open_world_corridor_proof` lane is a runnable large-world candidate surface at the declared
20M scale, but it is not yet promoted `real_chunked` proof evidence, and the rest of the
canonical open-world ladder remains staged-contract-only until it exposes benchmark-consumable
asset paths.

## Outputs

Default output directory:

`tests/output/benchmark_suite/<timestamp>/`

Generated artifacts:

- `benchmark_suite_report.json`
- `benchmark_suite_summary.md`
- `<lane_id>.json` per lane
- `<lane_id>.log` per lane
- optional dashboard artifacts (`benchmark_suite_dashboard.html`, `benchmark_suite_*.svg`)

The public docs surface should prefer the snapshot table above for the current committed result and keep the charts focused on the exported lane data.

<figure markdown="1">
![Diagram showing how the benchmark runner produces reports, dashboards, charts, and optional capture outputs](../assets/images/benchmark-artifacts-map.svg){ .gs-diagram }
<figcaption>The benchmark runner is one orchestrated entrypoint that fans out into JSON, dashboard HTML, SVG charts, and optional capture directories for visual proof.</figcaption>
</figure>

## Interactive Performance Charts

!!! info "Data source"
    Charts below render from `assets/data/benchmark_latest.json`, generated by `scripts/export_benchmark_vegalite.py` during the docs build. If no benchmark data is available, charts will show an empty state.

### Lane Scores

```vegalite
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "data": {"url": "../assets/data/benchmark_latest.json"},
  "mark": {"type": "bar", "tooltip": true},
  "encoding": {
    "y": {"field": "lane_id", "type": "nominal", "sort": "-x", "title": "Lane"},
    "x": {"field": "score", "type": "quantitative", "title": "Score"},
    "color": {"field": "lane_id", "type": "nominal", "legend": null},
    "tooltip": [
      {"field": "lane_id", "title": "Lane"},
      {"field": "score", "title": "Score", "format": ".1f"},
      {"field": "avg_fps", "title": "Avg FPS", "format": ".1f"}
    ]
  },
  "width": "container",
  "height": 200,
  "title": "Benchmark Lane Scores"
}
```

## How to Update

1. Run a benchmark: `python tests/runtime/run_benchmark.py --profile everything`
2. Export data: `python scripts/export_benchmark_vegalite.py`
3. Refresh the snapshot and coverage tables in `docs/performance/index.md` when new committed results are available.
4. Build docs: `python scripts/build_docs_site.py --strict`

### Average FPS by Lane

```vegalite
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "data": {"url": "../assets/data/benchmark_latest.json"},
  "mark": {"type": "bar", "tooltip": true},
  "encoding": {
    "y": {"field": "lane_id", "type": "nominal", "sort": "-x", "title": "Lane"},
    "x": {"field": "avg_fps", "type": "quantitative", "title": "Average FPS"},
    "color": {"value": "#c96b2c"},
    "tooltip": [
      {"field": "lane_id", "title": "Lane"},
      {"field": "avg_fps", "title": "FPS", "format": ".1f"},
      {"field": "p99_frame_ms", "title": "P99 ms", "format": ".2f"}
    ]
  },
  "width": "container",
  "height": 200,
  "title": "Average FPS per Benchmark Lane"
}
```
