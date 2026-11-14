# D3-5 producer captures (pending)

Phase 1 deliberately contains no JSON fixture. The positive Python tests fail
explicitly until the real producer has passed on CI and its captures are added.
Do not author JSON to satisfy these tests or capture the phase-1 red export.

Producer: `modules/gaussian_splatting/tests/test_d3_5_telemetry.cpp`, built with
`tests=yes`, running the strict `GaussianSplatting [WorldIO]` lane in
`tests/ci/run_module_tests.py`. The tag is
`[GaussianSplatting][HLOD][WorldIO][D3Telemetry]`; no RenderingDevice is needed.
The HLOD fixture uses the production V4 GSQ writer, paged source, shared page
store, real workers and packer. Only device writes use the existing CPU sink.
The chunk fixture uses the production paged GSQ writer and captures on a worker.

After phase 2, use a **pull request targeting master** to obtain the first
capture. The CPU job depends only on `changes`, so the red missing-fixture guard
does not block it. PR/merge-group runs use `--skip-guards` and reach WorldIO;
manual/nightly runs execute the full guard phase and stop before WorldIO until
the fixture exists. No workflow or gate change is needed for this bootstrap.

Download **`baseline-qa-cpu-results`** from the `cpu-tests` job of
`.github/workflows/baseline_qa.yml`. Its existing root `*test_results.json`
upload runs after the module lane, including when the job fails. Copy these
files verbatim into this directory only after inspecting the executed cases and
assertions in the same run:

| Artifact member / future fixture | Contents |
| --- | --- |
| `d3_5_hlod_test_results.json` | Verbatim renderer dictionaries at mailbox, ready, partial-upload, completed and replaced-source states; independently observed source generations |
| `d3_5_chunk_test_results.json` | Verbatim `get_decode_stats()` dictionaries for checked and skipped digest captures; independently observed source generations |
| `d3_5_delays_test_results.json` | Real HLOD telemetry for control, reader-delay and decode-delay arms |

The producer also writes identical local copies under `tests/output/d3_5/`;
that directory is **not** uploaded by this job. The root copies above are the
uploaded artifact members. Add the workflow run URL, attempt, immutable binary
commit, exact case command, executed/failed case counts and SHA-256 of each
captured file here when capturing, following `../d2_real_scan/README.md`.
No CI run, binary provenance or file hashes are claimed yet.

The contract names in the tests are policy. `payload_telemetry` is the explicit
HLOD renderer/probe block; the chunk source exports its own flat block.
`route` is `hlod` or `chunk`. `source_generation` and `collection_epoch` identify
the immutable source instance (`paged_source->get_instance_id()` for HLOD,
source instance ID for chunk); replacement starts a fresh measurement collection.
Times use ns, bytes use bytes, stage scope is `worker_cpu_sum`, and verification
has a nonempty `verify_scope`. `decode_nsec` includes verification and performed
decoded digest; `decode_remainder_nsec` subtracts those parts. Pack is separate.
`digest_status=skipped` carries `digest_nsec=null`, never a free 0 ns measurement.
Upload bytes/slices count actual atlas payload writes; metadata uses the shared
allowance but is not atlas payload. Queue residence includes the ready hold;
upload completion latency spans the first slice through final metadata, with
an independent hold during partial transfer. Optional unmeasured VRAM is absent/null.

Phase 2 must implement the TESTS_ENABLED dynamic renderer seam
`_test_set_payload_telemetry_delays({read_usec, decode_usec}) -> Error`. It delays
actual reader and decode execution independently, outside packing. Telemetry
must expose observed hook calls and elapsed ns as `test_read_delay_calls`,
`test_read_delay_nsec`, `test_decode_delay_calls`, `test_decode_delay_nsec`.
The existing read-service hook is private behind the page store, and there is
no existing decode delay hook. Phase 1 adds neither a production hook nor fake
stage measurements; its guarded dynamic lookup fails on an assertion at base.

For a later probe capture without workflow changes, the existing artifact is
`beyond-vram-flythrough-A-<run_id>-<run_attempt>`, member
`probe/A/path_run/result.json` (`$RUNNER_TEMP/bvw-out/` upload). Phase 2 must retain
the explicit route block in result JSON; chunk-shaped fallback zeros are invalid.
