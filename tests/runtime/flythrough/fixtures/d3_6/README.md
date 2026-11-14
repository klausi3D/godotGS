# D3-6 real T0 extracts

The camera, drop and migration extracts are small JSON projections of synthetic
T0 producer output from workflow run **43602195987**, attempt 1, head
`6721bc153c3edfdb883756681b650cd8c7485c81`. The source artifact is
`shared-layers-t0-none-43602195987-1`. Each extract records relative source paths,
JSON selectors and source-file SHA-256 values. No screenshots, binaries or full
logs are included. The decoded-content extract below comes from run
**43629131401**, attempt 1. Both downloaded sources were read without modification.

The old/new executions really ran on the 3080; these projections alone are not
complete paired evidence. The 43602195987 download has screenshots and JSON
summaries but no trace binaries. Unit tests combine extracts with explicitly constructed
comparison-policy preconditions and the older captured BV-07 trace; that does
not constitute a new GPU execution.

## Camera identity

[Camera extract](run43602195987_camera.json):
`chunk/arm.json` at `requested.camera_poses[11].table[0][5]` is `-0.0`, while
`observed.settings.camera_poses[11].table[0][5]` is `0.0`. The old JSON-text
hashes differ (`648e5f75...` versus `e43fe59b...`) although the arrays are
numerically equal. The digest was derived by Python from `camera_poses`, not
emitted directly in `result.json`. Both arms have `observed.camera_application`
with `matches=true`: chunk checked 26,604 rows, HLOD 25,348 rows. The replacement
fingerprint hashes typed JSON with numbers encoded as little-endian f32 hex,
strings/keys as UTF-8 hex, and signed zero normalized after f32 conversion. It
is also emitted by the process. Coordinate changes still fail exact comparison.

## Genuine overlap-record loss

[Drop extract](run43602195987_drops.json) retains values from
`{chunk,hlod}/arm.json:probe.overlap_drops_whole_run` and
`probe.passes[*].overlap_drops_{checkpoints,moving}`, with segment metadata from
`result.json:segments`.

| Route / pass / window | Drop intervals | Sampled lost records | First receiving trace row / segment |
| --- | ---: | ---: | --- |
| chunk cold checkpoint | 1 | 231,437 | 13,332 / S12 teleport |
| chunk repeat checkpoint | 2 | 231,399 | 13,334 / S0 hold |
| chunk repeat movement | 2 | 4,429 | 18,053 / S5 move |
| HLOD cold movement | 11 | 38,563 | 3,525 / S4 move |
| HLOD repeat movement | 10 | 39,258 | 15,468 / S4 move |

Chunk totals are **5 intervals / 467,265 sampled lost records**, all
`binning_budget_drop_events`. HLOD totals are **21 / 77,821**, all
`raster_tile_cap_drop_events`. HLOD checkpoint drops are zero. These are
image-affecting overlap-record losses, not eviction or cancellation. Counters
arrive through asynchronous GPU readback: the receiving row is not necessarily
the causing GPU frame. Only each window's first event row is retained in these
summaries; the absent trace binaries prevent listing every receiving row.

## Migration observations

[Migration extract](run43602195987_migration.json) retains exact
`migration/{base,candidate}/arm.json:payload_behavior`, selected native
`result.json:final_render_stats`, world readback and checkpoint row indices.

| Comparison | Checkpoint | Base / cold | Candidate / repeat | Delta |
| --- | --- | ---: | ---: | ---: |
| old versus new, cold | C2 | 204,115 | 206,746 | +2,631 |
| old versus new, repeat | C7 | 603,152 | 355,083 | -248,069 |
| same old binary, cold versus repeat | C7 | 355,083 | 603,152 | +248,069 |
| same new binary, cold versus repeat | C2 | 206,746 | 204,115 | -2,631 |

Source checkpoint indices/rows: old C2 `[2]`/5,365; new C2 `[2]`/5,279 and
`[10]`/17,326; old C7 `[7]`/13,497 and `[15]`/25,855; new C7
`[15]`/25,615. Drawn-node counts match at all 16 corresponding checkpoints.
All listed checkpoints report `settle.settled=true`.

Both processes loaded V4 SHA-256
`1fcf238de9b758439e9c673f94742b006958b4a9d58ca069040aa48dbd7acc70`.
Native read requests are 629/618, stored bytes 210,258,174/207,300,796, upload
bytes 879,393,328/872,497,296, resident atlas pages 5,131/5,136 at capacity
5,136/5,136. Scheduling, cancellation, eviction and rereads can change these
aggregate totals. Both have zero read failures, failed loads/uploads,
uncancelled late drops and nonresident draws. Cancelled late completions are
reported separately and are not overlap-record drops.

The comparator requires the exact source digest, actual HLOD route, matching V4
shape, positive common payload work, zero common failures and complete planned
cold/repeat checkpoint coverage. Cross-binary draw differences remain failures
unless the base varies across its cold/repeat passes and every candidate value
lies within the base's observed range; that case is reported as inconclusive.
Candidate-only variation does not excuse a difference. Missing per-payload decoded bytes, pages,
splats and digest stay explicitly not measured for immutable pre-D3. A source
file SHA is not a measured decoded-payload digest.

## Decoded content identity

[Content extract](run43629131401_content.json) comes from workflow run
**43629131401**, attempt **1**, head
`2741a2ac4a086555a70988086244b55d9315dd01`, artifact
`shared-layers-t0-none-43629131401-1`. Its source is the **129,424,128-byte**
`experiment.json`, SHA-256
`585b276f0babde976a8df254e3b73766bb0edc5dbedef1969dff953b5a33ee46`.
A one-off standard-library extraction script outside the repository ran with
`python -I`, checked the source SHA-256 and head, and verified every retained
JSON value against the source after serialization. The fixture's provenance
block records the source directory, exact selectors, capture statement and trims.

The **510,017-byte** fixture retains all **6,308** digest entries across these
complete maps, including the native telemetry copies that the comparator
cross-checks. No node map was sampled or edited despite the size exceeding
400 KB. `prepared.content_reference_sources` is also retained unchanged.

| Observation | Reference nodes | Runtime nodes | Native telemetry nodes |
| --- | ---: | ---: | ---: |
| Chunk | 1,284 | 1,284 | 1,284 |
| HLOD | 614 | 614 | 614 |
| Migration candidate | same HLOD reference | 614 | not retained; comparator consumes `payload_behavior.content_digests` |
| Migration base | unavailable | observed `null` | not retained |

Chunk references come from the prepared GSQ page directory; HLOD references
come from independent prepare/W13 verification. Every captured runtime node
matches its route's reference. The migration base's
`payload_behavior.content_digests` was present and explicitly `null`; it was
not filled in. Migration content identity remains **NOT_MEASURED**, while the
candidate content check is MATCHED. Cross-route equality is not claimed.

Both migration `payload_behavior.drawn_counts` arrays are small and retained
in full (16 records each). They demonstrate candidate-only jitter:

| Checkpoint | Base cold / repeat | Candidate cold / repeat |
| --- | ---: | ---: |
| C4 | 948,672 / 948,672 | 948,672 / 145,052 |
| C7 | 355,083 / 355,083 | 355,083 / 601,608 |

All other experiment fields were trimmed: camera/settings data, traces and
paths, screenshots, frame/pass measurements, other telemetry and payload
behavior, and source/binary receipts. The head is retained in provenance.
No digest or draw record was normalized, synthesized or altered.

`test_captured_runtime_content_digests_match_preparation` imports the captured
maps into the existing synthetic policy envelope and checks complete node
counts. Its malformed-input cases swap two captured HLOD digests, replace a
chunk digest with another captured digest, and omit the candidate migration map.
Both runtime copies are mutated together where present, so the independent
reference must catch wrong content. The companion
`test_captured_candidate_only_c4_c7_draw_jitter_is_rejected` imports the actual
draw counts and requires both checkpoints to be mismatches.

These constructed tests do not change the original run's verdict: its report
was **UNUSABLE** for real chunk/HLOD `drop_events` (#1444) and migration draw
differences. A passing content check does not excuse those failures or certify
a new paired GPU execution.
