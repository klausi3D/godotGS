# D3-5 producer captures (captured)

Verbatim producer output, captured once and checked in so the probe-lane tests can
check the route-labelled telemetry validator without a build (`tests/AGENTS.md`:
never hand-author a fixture that claims to model a producer's output). Re-capture
them, do not edit them, when the export changes. Malformed-input cases in
`test_probe_lane.py` mutate deep copies; these files stay unchanged.

Captured from workflow run
[43580666400](https://github.com/klausi3D/godotGS/actions/runs/43580666400), attempt 1,
job `CPU-Only QA Tests (Linux, hosted)` of `.github/workflows/baseline_qa.yml`,
artifact `baseline-qa-cpu-results` (root `*test_results.json` members). The
tests-enabled binary was built from head `ad018e00cba`. The producer is
`modules/gaussian_splatting/tests/test_d3_5_telemetry.cpp`, tag
`[GaussianSplatting][HLOD][WorldIO][D3Telemetry]`, run by the strict
`GaussianSplatting [WorldIO]` lane of `tests/ci/run_module_tests.py`; that lane
passed in the same run (314 cases, 0 failed), including the three D3-5 cases.

| File | Producer | SHA-256 |
| --- | --- | --- |
| `d3_5_hlod_test_results.json` | Renderer `build_render_stats()` over the real V4 GSQ paged source, shared page store, workers and packer (device writes go to the existing CPU sink) at mailbox, ready, partial-upload, completed and replaced-source states, with independently observed source generations | `14472dbd1a9ff2199c2909be653bbe943e41c8d4dbc551de73b98c6e1ed504b8` |
| `d3_5_chunk_test_results.json` | Chunk GSQ source `get_decode_stats()` for a checked and a skipped decoded-digest capture | `db6002b7aaaef82e815db051d6209e32262a793ddc10fee373557a3e1aef76c3` |
| `d3_5_delays_test_results.json` | HLOD `payload_telemetry` for control, reader-delay and decode-delay arms | `3ea603a4a4e495c2a5ff22cd05e0a9d661aec3147489280c1698e22ca513556b` |

The files hold counters, durations, labels and renderer status strings only: no
splat payload, no scan data, no machine paths.

## Contract

The contract names in the tests are policy. `payload_telemetry` is the explicit
HLOD renderer/probe block; the chunk source exports its own flat block. `route`
is `hlod` or `chunk`. `source_generation` and `collection_epoch` identify the
immutable source instance (source object instance ID); replacement starts a fresh
measurement collection. Times are ns, bytes are bytes, stage scope is
`worker_cpu_sum`, and verification names its `verify_scope`. `decode_nsec`
includes verification and any performed decoded digest; `decode_remainder_nsec`
subtracts those parts (it is not zstd time). Pack is separate.
`digest_status=skipped` carries `digest_nsec=null`, never a free 0 ns
measurement. Upload bytes/slices count actual atlas payload writes. Optional
unmeasured VRAM is absent or null, never 0 (#1441).

The test-only renderer seam `_test_set_payload_telemetry_delays({read_usec,
decode_usec})` delays actual reader and decode execution independently; telemetry
reports the observed hook calls and elapsed ns (`test_read_delay_*`,
`test_decode_delay_*`). In the captured delay arms the reader delay appears in
`read_latency_nsec` and the decode delay in `decode_nsec`.
