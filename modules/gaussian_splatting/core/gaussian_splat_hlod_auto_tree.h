#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"

// D2-3 driver: GsqAutoTreeSource -> TiledAutoTree -> WriterV4 (NONE or GSQ v1)
// -> reopen every node, recompute metrics and compare pre-finalize digests ->
// stage output and <stem>.manifest.json, then publish with per-file atomic
// replacements and rollback on ordinary I/O errors. Native filesystems cannot
// atomically replace TWO fixed names together: a crash/concurrent reader can see
// a mixed pair. Hard-link backups preserve previous bytes until publication;
// unsupported filesystems fail closed. Rollback filesystem failures preserve
// recovery files and name their directory in the error, rather than deleting old
// data. Callers must exclude external writers and validate the sidecar file SHA.
// Only owned private staging/scratch directories are cleaned, never caller files.
// codec="none" (default) preserves uncompressed output. codec="gsq" uses
// gsq_v1_codec_params() for scratch and V4 output (header codec id 1).
// Individual codec parameter overrides are not exposed by the bound API.
//
// Manifest schema godotgs-auto-tree-manifest/1:
// tree_sha, source_fingerprint, input_file_sha256, input_manifest_sha256,
// input_gen_sha, input_generator_params_sha (when available), build_params,
// codec, nodes=[{stored_sha256, decoded_sha256}], totals,
// levels=[{depth,node_count,eps_min,eps_median,eps_max}], content_generation,
// output_file_sha256 and report. content_generation is an unsigned decimal
// string (JSON numbers cannot retain all 64 bits). The codec block is unchanged
// for none: {"codec":"none"}. GSQ records flat fields codec="gsq", codec_id,
// sh_bits (three integers), opacity_bits, pos_step_m and zstd_level.
// GSQ alone also records header_bounds={position_f32_bits:[x,y,z],
// size_f32_bits:[x,y,z]} from the reopened output header. Each entry is the
// unsigned IEEE754 float32 bit pattern stored as an integer, so JSON preserves
// the exact written bounds. This output metadata is outside the tree_sha key;
// output_file_sha256 covers the header. Codec-none manifest content is unchanged.
// GSQ decoded_sha256 is gs_gsq::decoded_digest of read_node_payload's runtime
// form (absolute leaves, relative interiors), checked against one encode/decode
// of the original bake in V4's node-relative frame. Codec none retains its
// existing raw Gaussian bytes followed by high-order SH bytes digest.
// stored_sha256 hashes the entire V4 record for either codec.
//
// tree_sha hashes UTF-8 JSON::stringify(key, "", true, true), without newline,
// where key={source_fingerprint,build_params,codec,input_file_sha256} and
// build_params excludes worker_count. Record worker_count in manifest build_params
// and report, but exclude it from the key (schedule independence, design section 6).
// Frozen interior fields: merge_grid, reduction, node_max_splats, eps_growth,
// max_eps_steps. Canonical numeric types are INT for leaf_max, max_tile_splats,
// merge_grid, node_max_splats, max_eps_steps; FLOAT for reduction and eps_growth.
// The entire codec block participates in the key. GSQ codec_id, sh_bits entries,
// opacity_bits and zstd_level are INT; pos_step_m is FLOAT (double of the
// CodecParams float, without decimal rounding). Codec none has no numeric fields.
// Restore these schema types after JSON parsing before recomputing tree_sha.
// Limits, paths, timings and memory are execution metadata only.
// wall_usec measures through verified staging, excluding final manifest write
// and publication so the returned and persisted report are identical. Tile
// payloads are spilled by the coordinator as batches drain; only node tables and
// a bounded tile/merge working set stay resident. Original frontier payloads use
// separate lossless scratch, so stitching never consumes GSQ quantization.
// GSQ additionally reserves per-node digests and bounded single-node verification
// encode/decode and reopen buffers, separately from WriterV4's working memory.
// Successful reports include scratch_bytes_total (cumulative spill and frontier
// bytes) and scratch_bytes_peak (maximum simultaneous scratch plus staged output
// file lengths, excluding the final manifest). These are measured logical lengths,
// not allocated filesystem blocks; missing measurements are omitted on failure.
class GaussianSplatHlodAutoTree : public RefCounted {
	GDCLASS(GaussianSplatHlodAutoTree, RefCounted);

protected:
	static void _bind_methods();

public:
	static Dictionary build(const String &p_input_path, const String &p_output_path, const Dictionary &p_options);
#ifdef TESTS_ENABLED
	// Unbound C++ identity oracle: old retained BakeResult -> BFS/bucket-0 writer
	// route. Its payloads come from the same in-memory sink exercised by D2-2's
	// independent in-core identity tests; no release/script option enables it.
	static Dictionary build_retained_for_test(const String &p_input_path, const String &p_output_path, const Dictionary &p_options);
#endif
	// Host-only metadata oracle. Each call owns a fresh, unpublished snapshot;
	// leaves_only forces stale interiors as a diagnostic control, not a verdict.
	static Dictionary oracle_cut(const String &p_v4_path, const Array &p_poses, const Dictionary &p_options);
};
