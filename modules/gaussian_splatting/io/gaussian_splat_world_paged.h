#ifndef GAUSSIAN_SPLAT_WORLD_PAGED_H
#define GAUSSIAN_SPLAT_WORLD_PAGED_H

// ADR docs/architecture/adr-hlod-streaming.md section 14.4, package E1: the V4 paged HLOD package.
//
// V4 is a distinct, opt-in format identity (version word 4). It never reinterprets V3 bytes and the strict v1-v3
// readers are untouched: select_header() still refuses version 4, and this file is the only reader of it.
//
// Layout (little endian, every offset and count a checked unsigned 64-bit quantity, every region page aligned):
//
//   [0, 4096)                header page: identity, counts, region extents, index digest, header digest
//   [4096, +index_bytes)     index pages (64 KiB each): 255 fixed 256-byte node records per page, each page led by
//                            a 64-byte header carrying its own SHA-256 so one page can be verified alone
//   [payload_offset, ...)    one payload record per node, each starting on a 4096-byte boundary:
//                            64-byte record header + raw Gaussian/SH bytes (codec 0) or one GSQ page (codec 1)
//   [metadata_offset, ...)   optional UTF-8 JSON dictionary (world metadata), digest in the header
//
// A node record repeats the v3 topology and adds what the runtime snapshot used to derive by scanning the whole
// payload: the overlap footprint, the DC encoding, whether the node is DC-homogeneous and whether its centre
// reconstructs its positions exactly (the "snapshot scan" D1 listed as the reason the CPU copy of the payload
// stayed resident). With them the runtime needs no payload to describe a node; payloads are read on demand.
//
// Positions are stored node-relative exactly like the v3 gaussian section (leaf positions relative to the cell
// centre); read_node_payload() hands the runtime the same absolute float32 leaf positions the resident load
// produces, so a node packs bit-identically whichever way it was read.
//
// Integrity metadata never replaces semantic validation: every page and record is hashed AND its topology,
// finiteness, bounds and metrics are re-derived and compared. Failures carry a distinct reason string.

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_hlod_tree.h"
#include "core/error/error_list.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

#include <atomic>
#include <cstdint>

class GaussianSplatWorld;
class PayloadPageStore;
namespace gs_gsq {
class DecodeContext;
}

namespace gs_world_paged {

static constexpr uint32_t kMagic = 0x57505347u; // 'GSPW', shared with v1-v3.
static constexpr uint32_t kVersion = 4u;
static constexpr uint32_t kRecordMagic = 0x504E5347u; // 'GSNP'
static constexpr uint32_t kIndexPageMagic = 0x49505347u; // 'GSPI'
static constexpr uint64_t kPageBytes = 4096u;
static constexpr uint64_t kHeaderBytes = kPageBytes;
static constexpr uint64_t kIndexPageBytes = 65536u;
static constexpr uint64_t kIndexPageHeaderBytes = 64u;
static constexpr uint64_t kIndexRecordBytes = 256u;
static constexpr uint64_t kRecordsPerIndexPage = (kIndexPageBytes - kIndexPageHeaderBytes) / kIndexRecordBytes; // 255
static constexpr uint64_t kPayloadRecordHeaderBytes = 64u;
static constexpr uint32_t kMaxNodes = 1u << 20; // Reader bound; also the runtime's (GaussianSplatHlodRuntimeData::MAX_NODES).
static constexpr uint32_t kMaxHighOrder = 21u;
static constexpr uint32_t kFlagIs2D = 1u << 0u;
static constexpr uint32_t kFlagHasMetadata = 1u << 1u;
static constexpr uint32_t kKnownFlags = kFlagIs2D | kFlagHasMetadata;
// Node-record flag bits.
static constexpr uint32_t kNodeHomogeneousDc = 1u << 0u;
static constexpr uint32_t kNodeCenterExact = 1u << 1u;
static constexpr uint32_t kNodeKnownFlags = kNodeHomogeneousDc | kNodeCenterExact;
static constexpr uint64_t kMaxMetadataBytes = 16u * 1024u * 1024u;

inline uint64_t align_up_page(uint64_t p_value) {
	return (p_value + (kPageBytes - 1u)) & ~(kPageBytes - 1u);
}

// Per-node facts the runtime needs without the payload.
struct NodeMetrics {
	double footprint = 0.0; // sum over the payload of 36 * s[1] * s[2] (sorted axis scales), ADR 6.5
	uint32_t dc_encoding = 0u;
	bool homogeneous_dc = true;
	bool center_exact = true;
};

// Shared V4 serialization. Callers validate layout/counts before encoding.
struct HeaderFields {
	uint32_t flags = 0u;
	uint32_t leaf = 0u;
	uint32_t interior = 0u;
	uint32_t node_count = 0u;
	uint32_t sh_degree = 0u;
	uint32_t sh_first_order = 0u;
	uint32_t sh_high_order = 0u;
	uint32_t bake_rule = 0u;
	uint32_t codec_id = 0u; // Header u32 at byte 60; 0 = none, 1 = GSQ v1.
	double origin[3] = { 0.0, 0.0, 0.0 };
	Vector3 bounds_pos;
	Vector3 bounds_size;
	uint64_t index_offset = 0u;
	uint64_t index_page_count = 0u;
	uint64_t payload_offset = 0u;
	uint64_t payload_bytes = 0u;
	uint64_t metadata_offset = 0u;
	uint64_t metadata_size = 0u;
	uint64_t file_bytes = 0u;
	uint64_t content_generation = 0u;
	uint8_t index_digest[32] = {};
	uint8_t metadata_digest[32] = {};
};

void write_header(uint8_t *p_page, const HeaderFields &p_header);
bool record_stored_bytes(uint32_t p_count, uint32_t p_high_order, uint64_t &r_gaussian, uint64_t &r_sh, uint64_t &r_total);
// Envelope + GSQ header + worst-case zstd frame sizes. Zero for unsupported counts/degrees.
uint64_t gsq_record_stored_bytes_limit(uint32_t p_count, uint32_t p_sh_degree);
uint64_t gsq_decode_workspace_bytes();

// The V4 reader's node-relative -> absolute conversion for leaves. Writers use
// this on serialized positions before computing the metrics promised by the index.
// Interior payloads remain node-relative and must not pass through this helper.
void reconstruct_leaf_positions(Gaussian *p_gaussians, uint32_t p_count, const double p_cell_center[3]);

// Single definition of the metrics (writer and reader-side verification share it). Fails closed on a
// non-finite or negative scale / overflowing footprint; r_reason is a distinct string.
bool compute_node_metrics(const Gaussian *p_gaussians, uint32_t p_count, const double p_cell_center[3],
		bool p_is_leaf, NodeMetrics &r_metrics, String &r_reason);

// The finiteness / containment checks of the v3 HLOD payload validator, on node-relative positions. Shared with
// the v2/v3 loader (which calls it unchanged).
bool validate_node_payload(const double p_cell_center[3], uint32_t p_sh_high_order_count, const GaussianSplatHlodNode &p_node,
		const Gaussian *p_gaussians, const Vector3 *p_sh, String &r_reason);

} // namespace gs_world_paged

// An open V4 package. Immutable after open(); both read methods are thread-safe.
// Offline reads retain the independent direct-file path. Runtime workers use the
// renderer-owned common store's transport and transient exclusive decoder leases.
class GaussianSplatHlodPagedSource : public RefCounted {
public:
	struct NodeRecord {
		GaussianSplatHlodNode topology;
		gs_world_paged::NodeMetrics metrics;
		uint64_t offset = 0u; // payload record start, page aligned
		uint64_t stored_bytes = 0u; // envelope + raw Gaussian/SH or compressed GSQ page, no padding
		uint64_t extent_bytes = 0u; // stored_bytes rounded up to a page
		uint8_t digest[32] = {};
		double cell_center[3] = { 0.0, 0.0, 0.0 };
		bool is_leaf = false;
	};
	struct Info {
		uint32_t codec_id = UINT32_MAX; // Unopened sentinel; only a parsed, explicit zero means none.
		uint32_t leaf_splat_count = 0u;
		uint32_t interior_splat_count = 0u;
		uint32_t node_count = 0u;
		uint32_t sh_degree = 0u;
		uint32_t sh_first_order = 0u;
		uint32_t sh_high_order = 0u;
		bool is_2d = false;
		AABB bounds;
		uint64_t file_bytes = 0u;
		uint64_t index_bytes = 0u;
		uint64_t payload_bytes = 0u;
		uint64_t content_generation = 0u; // first 8 bytes of SHA-256(header identity fields + every node's body digest); stamped into every record
		uint32_t bake_rule_version = 0u;
	};
	struct Stats {
		uint64_t reads = 0u;
		uint64_t bytes_read = 0u;
		uint64_t read_failures = 0u;
		uint64_t largest_read_bytes = 0u;
	};

	// Single use: a second call on an opened source is refused (ERR_ALREADY_IN_USE, v4_already_open); use a new instance.
	// Validates the header, every index page and the whole topology (without reading any payload) and fills
	// r_tree with the node table (interior payload NOT resident). Every failure clears r_tree and r_metadata,
	// including ERR_ALREADY_IN_USE (which leaves the source itself open and unchanged). Nothing is published: the returned
	// error is OK / ERR_FILE_CORRUPT / ERR_FILE_UNRECOGNIZED / ERR_OUT_OF_MEMORY / a file error and r_reason is a
	// distinct machine-readable reason.
	Error open(const String &p_path, GaussianSplatHlodTree &r_tree, Dictionary &r_metadata, String &r_reason);
	~GaussianSplatHlodPagedSource();

	bool is_open() const { return opened; }
	const String &get_path() const { return path; }
	const Info &get_info() const { return info; }
	uint32_t get_codec_id() const { return info.codec_id; }
	const Vector<NodeRecord> &get_nodes() const { return nodes; }

	// Reads and verifies one node's payload and returns it in the runtime snapshot's convention (leaf positions
	// absolute float32, interior positions node-relative). Bounded: exactly the record's stored_bytes, which the
	// index pins to the raw size or bounds by the GSQ worst-case stored size (at most 16,384 splats).
	// A replaced or truncated file is refused (length, record stamp and digest). Thread-safe.
	Error read_node_payload(uint32_t p_node, Vector<Gaussian> &r_gaussians, Vector<Vector3> &r_sh, String &r_reason) const;

	// D3-2 runtime transport contract. The renderer owns the store through worker
	// drain; installing the immutable catalog never changes this source. GSQ uses
	// one full-record descriptor; codec-none uses bounded read-service windows.
	Error install_in_page_store(PayloadPageStore &p_store, uint64_t p_generation) const;
	// Same validation and decoded-digest policy as read_node_payload(), with the
	// supplied store owning transport, decoder leases and stage accounting.
	// The caller owns this optional observer. Calls run on the reading worker,
	// outside every store lock. Values describe allocated buffer capacities independently of
	// reservation counters; stored bytes are announced before service submission
	// to cover the service/stash lifetime, workspace is its conservative bound.
	// Successful return transfers decoded ownership to the caller; the last event
	// is (0, decoded bytes, 0), and the caller observes its eventual clear itself.
	struct ReadStorageObserver {
		virtual void storage_changed(uint64_t p_stored_bytes, uint64_t p_decoded_bytes, uint64_t p_workspace_bytes) = 0;
		virtual ~ReadStorageObserver() = default;
	};
	Error read_node_payload_from_store(PayloadPageStore &p_store, uint64_t p_requester, uint64_t p_ticket,
			uint32_t p_node, Vector<Gaussian> &r_gaussians, Vector<Vector3> &r_sh, String &r_reason,
			ReadStorageObserver *p_observer = nullptr) const;

	Stats get_stats() const;

	// Test seam: the largest payload record this package can ever read in one call.
	uint64_t get_max_record_bytes() const { return max_record_bytes; }
	// Streamer admission: stored buffer + decoded arrays + decoder workspace for
	// GSQ. Codec none covers one bounded service window AND decoded arrays. Excludes the
	// packed output (charged by the streamer) and idle pooled contexts.
	uint64_t get_node_read_working_set_bytes(uint32_t p_node) const;

	// Writes `p_world` (a resident, complete HLOD world) as a V4 package, atomically. Opt-in and explicit: no
	// save, import or bake path calls it. The world's payload is read through the runtime snapshot, so the same
	// admission limits and exactness checks apply; r_reason names the first violated one.
	static Error write_package(const GaussianSplatWorld &p_world, const String &p_path, String &r_reason);

	// Test seam: the digest of the header-declared index (what content_generation is cut from).
	void get_index_digest(uint8_t r_digest[32]) const;

private:
	// Only the idle list is locked; decoding never holds the mutex. Each lease
	// owns a ZSTD_DCtx exclusively. Idle retention is bounded, active leases by callers.
	mutable Mutex decoder_mutex;
	mutable LocalVector<gs_gsq::DecodeContext *> idle_decoders;
	// Guarded by decoder_mutex. Once installed for runtime, even concurrent
	// offline leases return without retaining uncharged idle scratch.
	mutable bool runtime_installed = false;
	gs_gsq::DecodeContext *_lease_decoder() const;
	void _return_decoder(gs_gsq::DecodeContext *p_decoder) const;
	bool opened = false;
	String path;
	Info info;
	Vector<NodeRecord> nodes;
	uint8_t index_digest[32] = {};
	uint32_t record_high_order = 0u;
	uint64_t max_record_bytes = 0u;
	mutable std::atomic<uint64_t> stat_reads{ 0u };
	mutable std::atomic<uint64_t> stat_bytes{ 0u };
	mutable std::atomic<uint64_t> stat_failures{ 0u };
	mutable std::atomic<uint64_t> stat_largest{ 0u };
};

namespace gs_world_paged {
// p_page holds kIndexPageBytes; p_records contains the next 1..255 records.
void write_index_page(uint8_t *p_page, uint32_t p_page_index, const GaussianSplatHlodPagedSource::NodeRecord *p_records, uint32_t p_count);
// p_header holds kPayloadRecordHeaderBytes. Returns false on size overflow.
bool write_payload_header(uint8_t *p_header, uint32_t p_index, const GaussianSplatHlodPagedSource::NodeRecord &p_record,
		uint32_t p_high_order, uint64_t p_content_generation);
} // namespace gs_world_paged

#endif // GAUSSIAN_SPLAT_WORLD_PAGED_H
