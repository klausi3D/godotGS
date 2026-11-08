#pragma once

#include "gs_atomic_file_transaction.h"
#include "gsq/gsq_codec.h"
#include "gsq/gsq_page_builder.h"

#include "../core/gaussian_data.h"

#include "core/error/error_list.h"
#include "core/math/aabb.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

#include <cstdint>

// Streaming world writer (docs/architecture/adr-beyond-vram-worlds.md §4.2-§4.5).
//
// BV-03a scope: the PLAIN_V3 target, codec "none", in both source modes, built on
// GSAtomicFileTransaction (BV-02). It writes a plain v3 .gsplatworld without ever
// holding the world in memory, and its output is byte-identical to
// ResourceSaver::save of the same world (§4.5; proven by the [WorldWriter]
// doctests).
//
// Not here yet (later packages, per §10): Limits, preflight, cancel, progress and
// the Report (BV-03b); PAGED_V4 (BV-18a). The API grows into the §4.3 signature as
// those land.
//
// Codec GSQ v1 (BV-C3, container BV-C5a). The codec stage is gs_gsq::PageBuilder
// (io/gsq/gsq_page_builder.h): it turns each appended chunk into importance-ordered,
// Morton-inside, constant-grouped pages that are encoded, decoded and validated
// before use (W12) and carry their stored and decoded digests (W15). With
// codec_id 1 the writer produces plain v3 with flag bit 7 (paged GSQ chunks; layout
// in gaussian_splat_world_format.h): one run of pages per chunk, in page order, then
// the paged chunk table, the page table (offset, size, extent, both digests per page)
// and the metadata. The codec block is written into the file's paged extension as
// given (W16). It needs write_chunk_table (the chunk table is the page directory)
// and SH degree <= 3 with the full high-order layout. Every source mode takes a
// single pass: the SH travels inside the pages. The file size is known only once
// finalize() returns OK.
//
// Usage. Declare the world up front in a PlainSpec; then feed it chunk by chunk in
// chunk order (W3). One "chunk" is one append unit: with write_chunk_table it is
// also one record of the plain chunk table, whose indices are the contiguous range
// of splats that chunk appended. Without it the units only frame the stream.
//
//     Writer w;
//     w.begin_plain(path, spec, CodecParams(), SourceMode::REITERABLE);
//     for (uint32_t pass = w.current_pass(); pass != Writer::kNoPass; pass = w.current_pass()) {
//         for (each chunk c, in the same order every pass) {
//             w.append_chunk(desc[c], gaussians[c], sh_high[c], count[c]);
//         }
//     }
//     w.finalize();
//
//   - REITERABLE: the source is walked once per pass. Pass 1 consumes the
//     gaussians, pass 2 (only when the world has high-order SH) consumes the SH;
//     the pointer a pass does not consume is ignored and may be null. Pass 2 must
//     replay the same chunk stream: a chunk whose count or descriptor differs from
//     pass 1 is refused. Nothing is spilled.
//   - SINGLE_PASS: one walk. Gaussians go to the output, high-order SH to a scratch
//     sibling that finalize() copies behind them and then deletes.
//
// A pass ends when it has received exactly `spec.chunk_count` chunks; the writer
// then advances on its own. Declared means exact (W4): a chunk beyond the
// declared count, a splat beyond `spec.splat_count`, a pass that ends short of
// `splat_count`, or finalize() before every pass has completed all fail with a
// "count_mismatch" message, and nothing is written to the final path.
//
// Invariants kept here (§4.4): W1 (nothing at the final path before finalize()
// returns OK; on any failure, abort() or the destructor the temp and scratch are
// removed and a pre-existing final file stays byte-intact), W2 (writes only grow
// the file, apart from the final header rewrite), W3, W4, W5 (RAM is the chunk
// table plus one 1 MiB slice; index lists are generated, never held), W6
// (structural validity at append time, with the plain loader's rules), W7
// (output bytes are a pure function of the spec and the appended stream), W9 (via
// GSAtomicFileTransaction) and the structural part of W11 (begin_plain refuses a
// spec the plain loader would refuse).
//
// Reader suspension (#714). On Windows an open reader denies the delete access
// the atomic replace needs, and a world that is being streamed keeps pooled read
// handles to exactly this file. finalize() therefore flushes first and then holds
// StagedFileChunkPayloadSource::ScopedReaderSuspend on the destination across the
// replace only. If in-flight reads do not drain within its bounded wait, the
// replace is not attempted: finalize() returns ERR_BUSY, removes the temp and
// leaves the destination as it was. (On POSIX the guard is a no-op: rename()
// replaces a file readers still hold open.)
//
// Not thread-safe: one writer is driven by one thread.
namespace gs_world_writer {

enum class Target : uint8_t {
	PLAIN_V3,
	PAGED_V4,
};

enum class SourceMode : uint8_t {
	REITERABLE,
	SINGLE_PASS,
};

// §4.3. With codec_id 0 ("none") the other fields must be zero so a caller cannot
// believe a codec was applied. With codec_id 1 (GSQ v1) every field is required
// and range-checked; gsq_v1_codec_params() returns the D-12 pre-registered block.
struct CodecParams {
	uint32_t codec_id = 0; // 0 none, 1 GSQ v1
	uint8_t sh_bits[3] = { 0, 0, 0 };
	uint8_t opacity_bits = 0;
	float pos_step_m = 0.0f;
	int zstd_level = 0;
};

constexpr uint32_t kCodecNone = 0u;
constexpr uint32_t kCodecGsqV1 = 1u;

// The D-12 pre-registered GSQ v1 parameters (sh bits 6/5/5, opacity 16, 0.25 mm, zstd 3).
CodecParams gsq_v1_codec_params();

// codec_id 1 only: the encoder parameters of a GSQ codec block, or ERR_INVALID_PARAMETER
// with a gsq_invalid_params reason. No field defaults: W16 compares the block as given.
Error gsq_encode_params(const CodecParams &p_codec, gs_gsq::EncodeParams &r_params, String &r_reason);

struct PlainSpec {
	uint32_t splat_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order = 0; // high-order SH coefficients (Vector3) per splat
	bool is_2d = false;
	AABB bounds;
	// Number of append_chunk() calls per pass. At least 1.
	uint32_t chunk_count = 0;
	// Write the plain chunk table (and its generated index ranges).
	bool write_chunk_table = false;
	// Written as JSON::stringify(metadata) with sorted keys, exactly like the
	// in-core saver; empty = none.
	Dictionary metadata;
};

// One chunk-table record's geometry. The index range is implied by the stream.
struct ChunkDesc {
	AABB bounds;
	Vector3 center;
	float radius = 0.0f;
};

class Writer {
public:
	static constexpr uint32_t kNoPass = 0u;

	enum class State : uint8_t {
		IDLE,
		OPEN,
		COMMITTED,
		ABORTED,
		FAILED,
	};

	Writer() = default;
	~Writer();

	Writer(const Writer &) = delete;
	Writer &operator=(const Writer &) = delete;

	Error begin_plain(const String &p_final_path, const PlainSpec &p_spec, const CodecParams &p_codec, SourceMode p_mode);

	// count may be 0 (an empty chunk). Pointers the current pass consumes must be
	// non-null when count > 0.
	Error append_chunk(const ChunkDesc &p_desc, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count);

	// 1 or 2 while a pass is accepting chunks; kNoPass once every pass is complete
	// or when the writer is not open.
	uint32_t current_pass() const;
	// 1, or 2 for a REITERABLE world with high-order SH. 0 when not open.
	uint32_t get_pass_count() const { return state == State::OPEN ? pass_count : 0u; }

	// Completes the file and atomically publishes it at the final path.
	Error finalize();
	// Removes the temp and scratch; never touches the final path. Idempotent.
	void abort();

	State get_state() const { return state; }
	bool is_open() const { return state == State::OPEN; }
	Error get_error() const { return error; }
	const String &get_error_message() const { return error_message; }
	// Size the finished file will have: valid after begin_plain() with codec none; with
	// GSQ v1 only after finalize() returned OK (0 before).
	uint64_t get_expected_file_size() const { return layout.file_size; }
	// GSQ v1: pages written so far, and the page builder's counters (valid after finalize()).
	uint32_t get_page_count() const { return page_count; }
	const gs_gsq::PageBuildStats &get_page_stats() const { return page_stats; }

#ifdef TESTS_ENABLED
	// Called once the real reader-suspend guard has been taken, immediately before
	// the replace. Returning false makes finalize() treat the guard as not drained,
	// so tests can drive the refusal branch without a stuck reader thread.
	using DrainTestHook = bool (*)(void *p_userdata);
	static DrainTestHook drain_test_hook;
	static void *drain_test_userdata;
#endif

private:
	struct Layout {
		uint32_t flags = 0;
		uint32_t header_chunk_count = 0;
		uint64_t gaussian_offset = 0;
		uint64_t gaussian_bytes = 0;
		uint64_t sh_offset = 0;
		uint64_t sh_bytes = 0;
		uint64_t chunk_table_offset = 0;
		uint64_t chunk_table_bytes = 0;
		uint64_t indices_offset = 0;
		uint64_t indices_bytes = 0;
		uint64_t metadata_offset = 0;
		uint64_t metadata_size = 0;
		uint64_t page_table_offset = 0; // GSQ v1 only
		uint64_t file_size = 0;
	};

	Error _fail(Error p_error, const String &p_message);
	Error _append_sliced(GSAtomicFileTransaction &p_txn, const uint8_t *p_data, uint64_t p_size);
	Error _end_pass();
	Error _copy_scratch_sh();
	Error _write_tail();
	Error _append_chunk_gsq(const ChunkDesc &p_desc, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count);
	// Writes the plain prefix and, with GSQ v1, the paged extension behind it; returns the bytes written.
	uint32_t _encode_header(uint8_t *r_header) const;
	void _encode_chunk_record(const ChunkDesc &p_desc, uint64_t p_first_index, uint32_t p_count, uint8_t *r_record) const;
	void _cleanup();

	State state = State::IDLE;
	Error error = OK;
	String error_message;
	String final_path;
	PlainSpec spec;
	SourceMode mode = SourceMode::REITERABLE;
	Layout layout;
	PackedByteArray metadata_bytes;

	uint32_t pass = kNoPass;
	uint32_t pass_count = 0;
	uint32_t chunks_this_pass = 0;
	uint64_t splats_this_pass = 0;

	// W5: the chunk table is the only per-world state held in RAM. Each entry is
	// the encoded 56-byte record (pass 2 replays against it) or, without a
	// written table, just the chunk's count.
	LocalVector<uint8_t> chunk_records;
	LocalVector<uint32_t> chunk_counts;

	GSAtomicFileTransaction output;
	GSAtomicFileTransaction sh_scratch; // SINGLE_PASS with high-order SH only

	// GSQ v1 (bit 7). W5: the page table is held like the chunk table, 112 B per page.
	bool gsq = false;
	CodecParams codec;
	gs_gsq::PageBuilder *page_builder = nullptr;
	LocalVector<gs_gsq::BuiltPage> built_pages; // one chunk's pages, reused
	LocalVector<uint8_t> page_records;
	uint32_t page_count = 0;
	gs_gsq::PageBuildStats page_stats;
};

} // namespace gs_world_writer
