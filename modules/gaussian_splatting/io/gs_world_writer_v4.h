#pragma once

#include "gaussian_splat_world_paged.h"
#include "gs_world_writer.h"

// BV-18a/BV-C4 out-of-core writer. CodecParams selects both scratch encoding and
// the V4 header codec id: none preserves the original bytes; GSQ emits one
// node-relative GSQ page per record. W13 post-assembly verification belongs to BV-18b.
//
// Append runtime-form bodies (absolute leaf positions, relative interiors) in
// any order. One scratch file per (bucket, depth), beneath the caller's scratch
// directory. At most kMaxOpenSpillSegments (64) native segment handles stay open;
// LRU eviction closes without deleting. Reopens append at the recorded length
// or read during assembly. Closed files remain owned until deletion/cleanup.
// Scratch is not fsynced or committed; only the final output is durable.
// GSQ pages preserve node order, are encoded, decoded and validated
// before append (W12/W14). Leaf metrics describe the reader's reconstruction
// after node-relative serialization; interior metrics use the decoded form.
// As in write_package (D-7), inexact centre reconstruction on that runtime
// form refuses with ERR_UNAVAILABLE / v4_center_reconstruction_not_exact;
// failure removes all owned scratch files and preserves the destination.
// Codec none spills losslessly. Finalization derives depth/centre/leaf from topology,
// reuses gs_hlod_validate_tree, requires canonical BFS and exactly one use of
// every handle, and writes bodies in BFS order. content_generation uses digests
// of the actual stored V4 output bodies (node-relative, GSQ-compressed when selected).
//
// W1: publish through GSAtomicFileTransaction only after successful assembly.
// Failure, abort and destruction remove all owned temp/segment files and leave
// any pre-existing destination byte-intact. Delete each segment immediately
// after copying its last body. A crash mid-assembly is NOT resumable: the cache
// owner must sweep orphan files and re-bake. The writer owns only its files.
// Like the plain writer, this inherits GSAtomicFileTransaction's POSIX caveat:
// a directory-fsync error after rename reports a durability failure with the
// new destination already in place; it cannot roll back the atomic replace.
//
// Limits retain the plain writer's required fields and reasons: limits_unset,
// limit_scratch, limit_output, limit_ram, deadline (ERR_TIMEOUT), cancelled
// (ERR_SKIP). Check stop requests within <= 1 MiB of I/O and before commit.
// RAM admission conservatively includes serialization buffers and codec working
// contexts. Metadata cycles and Object/Callable/Signal values are refused before
// JSON stringification: arbitrary user-code stringification has no memory bound.
// One driving thread; only request_cancel/get_progress may cross threads.
namespace gs_world_writer {

// Segment descriptors only; the output transaction uses one additional handle.
constexpr uint32_t kMaxOpenSpillSegments = 64;

struct PagedV4Spec {
	uint32_t node_count = 0; // Final BFS table count; excess spills are diagnosed at finalize as unused handles.
	uint32_t leaf_splat_count = 0;
	uint32_t interior_splat_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order = 0;
	bool is_2d = false;
	double origin[3] = {};
	uint32_t bake_rule_version = gs_hlod::kBakeRuleVersion;
	AABB bounds;
	Dictionary metadata;
};

struct V4SegmentKey {
	uint64_t bucket = 0; // Tile id; stitched levels have their own bucket.
	uint32_t depth = 0; // Root = 0, NOT GaussianSplatHlodNode::height.
	bool operator==(const V4SegmentKey &p_other) const { return bucket == p_other.bucket && depth == p_other.depth; }
};

struct V4BodyDesc {
	V4SegmentKey segment;
	double cell_center[3] = {};
	bool is_leaf = false;
};

struct V4BodyHandle {
	uint64_t value = UINT64_MAX; // Opaque, scoped to one begin_paged().
};

struct V4Node {
	GaussianSplatHlodNode topology;
	V4BodyHandle body;
};

struct V4SpillRecord {
	V4BodyDesc desc;
	uint32_t splat_count = 0;
	gs_world_paged::NodeMetrics metrics; // Output reader runtime form, including leaf position round trip.
	String segment_path;
	uint64_t offset = 0; // Stored body range within segment_path.
	uint64_t stored_bytes = 0;
	uint8_t stored_digest[32] = {}; // SHA-256 of the entire stored body range.
	uint8_t output_body_digest[32] = {}; // Legacy scratch audit: decoded scratch, node-relative Gaussian + SH bytes.
};

struct V4SegmentReport {
	V4SegmentKey key;
	String path;
	uint64_t spill_bytes = 0; // Actual file length including spill framing, retained after deletion.
	uint32_t body_count = 0;
};

struct V4Report {
	Error error = OK;
	String reason;
	bool committed = false;
	LocalVector<V4SegmentReport> segments;
	uint64_t total_scratch_bytes = 0; // Cumulative segment file lengths, not remaining live bytes.
	uint64_t peak_scratch_plus_output_bytes = 0; // Maximum live logical lengths, includes output temp.
	uint32_t segments_deleted = 0;
	uint32_t nodes = 0; // Successfully appended bodies.
	uint64_t output_bytes = 0;
};

class WriterV4 {
public:
	WriterV4();
	~WriterV4();
	WriterV4(const WriterV4 &) = delete;
	WriterV4 &operator=(const WriterV4 &) = delete;

	Error begin_paged(const String &p_final_path, const String &p_scratch_dir, const PagedV4Spec &p_spec,
			const CodecParams &p_codec, const Limits &p_limits);
	// Homogeneous GSQ bodies use encode_page on the complete node in caller
	// order (a V4 node fits a GSQ page). No Morton/importance reordering here.
	Error append_node(const V4BodyDesc &p_desc, const Gaussian *p_gaussians, const Vector3 *p_sh_high,
			uint32_t p_count, V4BodyHandle &r_handle);
	// Inspect append-time facts without loading the body; retained through commit/failure.
	Error get_spill_record(V4BodyHandle p_handle, V4SpillRecord &r_record) const;
	// Refusals (ERR_INVALID_DATA): v4_handle_unused, v4_handle_reused,
	// v4_depth_mismatch, v4_cell_center_mismatch, v4_is_leaf_mismatch,
	// v4_children_not_contiguous, v4_parent_not_before_child. Handle multiplicity
	// is checked before node/spill matching; parent ordering before child ranges.
	Error finalize(const LocalVector<V4Node> &p_nodes);
	void abort();
	void request_cancel();
	// Reuses Progress: chunks = nodes; pass_count = 1; bytes_written includes scratch.
	Progress get_progress() const;
	const V4Report &get_report() const { return report; }

#ifdef TESTS_ENABLED
	// Set before begin_paged(); may only lower the production cap (minimum one).
	void set_max_open_segments_for_test(uint32_t p_max);
	struct SegmentHandleStats {
		uint32_t open_segments = 0;
		uint32_t peak_spill = 0;
		uint32_t peak_assembly = 0;
		uint64_t spill_reopens = 0;
		uint64_t assembly_reopens = 0;
	};
	// Reset at begin; retained after failure/commit. Peaks count EVERY open,
	// including opens immediately closed again on error. Driving thread only.
	SegmentHandleStats get_segment_handle_stats_for_test() const;

	// Called synchronously for EVERY copied body, after any last-use segment
	// deletion, before the next body's I/O or commit. May request_cancel(); must
	// not otherwise re-enter. Paths remain in Report after deletion. Instance-local
	// so a failed test cannot leave a global callback pointing to dead stack data.
	struct AssemblyEvent {
		uint32_t node_index = 0;
		String output_temp_path;
		uint64_t output_bytes = 0; // Logical length at this boundary.
	};
	using AssemblyObserver = void (*)(const AssemblyEvent &, void *);
	void set_assembly_observer(AssemblyObserver p_observer, void *p_userdata);
#endif

private:
	struct Impl;
	Impl *impl = nullptr;
	void _cleanup();
	Error _fail(Error p_error, const String &p_reason);
	Error _check_stop();
	Error _check_ram(uint64_t p_body_bytes = 0, uint32_t p_extra_records = 0);
	Error _open_segment(uint32_t p_segment, bool p_read_only);
	// UINT32_MAX selects the output transaction; otherwise selects a scratch segment.
	Error _write(const uint8_t *p_bytes, uint64_t p_size, uint32_t p_segment = UINT32_MAX);
	Error _read(uint32_t p_segment, uint64_t p_offset, uint8_t *p_bytes, uint64_t p_size);
	V4Report report;
};

} // namespace gs_world_writer
