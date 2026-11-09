#pragma once

// GSQ v1 page codec core (BV-C1a; docs/architecture/adr-beyond-vram-worlds.md D-12).
//
// One page is one plain-v3 chunk or one V4 node payload. It is split into 4,096-splat
// sub-blocks in the caller's order, so a prefix stays a valid sample and any range decodes
// without touching the other sub-blocks. Each sub-block is one zstd frame of SoA byte planes.
//
// Page layout (little endian, 256 B of header in front of the frames):
//
//   0   u32   magic "GSQ1"
//   4   u16   codec id (1)
//   6   u8    stored SH degree (0..3)
//   7   u8    flags (bit 0: 21-bit positions, bit 1: per-splat normals)
//   8   u32   splat count (1..65536)
//   12  u32   stored bytes, header included
//   16  u8[3] SH bits per band, then u8 opacity bits
//   20  u16   splats per sub-block (4096), u16 sub-block count
//   24  u8[8] reserved, zero
//   32  u8[32] SHA-256 of the stored bytes: [0, 32) then [64, stored bytes)
//   64  u32[16] stored size of each zstd frame; unused entries are zero
//   128 f32[6] position min xyz, max xyz
//   152 i32[6] log2 scale min xyz, max xyz, Q16
//   176 f32[6] DC min rgb, max rgb
//   200 f32[3] largest SH magnitude per band
//   212 f32[3] page normal (the value of every splat when bit 1 is clear)
//   224 f32[5] page area, DC alpha, stroke age, brush axes
//   244 u32[2] page painterly_meta, render_meta
//   252 u32   reserved, zero
//
// Sub-block planes, in this order (n = splats in the sub-block; each plane is n bytes):
//
//   position   2 or 3 planes per axis: zigzag delta of the page-relative integer, the first
//              splat of the sub-block against 0
//   scale      x, y, z low bytes, then x | y << 2 | z << 4 of the 10-bit high bits
//   rotation   a, b, c low bytes, then a | b << 2 | c << 4 | index << 6
//   opacity    2 planes
//   DC         r, g, b low bytes, then r | g << 3 | b << 6 of the 11/11/10-bit high bits
//   SH         one plane per coefficient, biased signed integer, zero is exact
//   normal     2 planes per axis, only with flag bit 1
//
// Determinism (risk K10): the module's GCC/Clang flags include -ffast-math, and a digest that
// depends on a floating point result is exposed to that and to contraction. So no value that
// reaches the page or a decoded float passes through floating point arithmetic: floats are
// read and built by their bit patterns, and everything between is integer. This TU also builds
// in its own environment, without the module's optimization flags (SCsub). The one floating
// point call, the sqrt seed in the .cpp, is corrected in integers, so its result cannot vary.
//
// Refusals are explicit: a field the page cannot carry (a per-splat area, degree 4 SH) fails
// with a gsq_* reason and is never dropped. Reasons are returned, not printed, so a corrupt
// page on a worker thread costs one failed decode and no log flood.

#include "../../core/gaussian_data.h"

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

#include <cstdint>

struct ZSTD_DCtx_s;

namespace gs_gsq {

constexpr uint32_t kCodecId = 1u;
constexpr uint32_t kSubBlockSplats = 4096u;
constexpr uint32_t kMaxPageSplats = 65536u;
constexpr uint32_t kMaxSubBlocks = 16u;
constexpr uint32_t kHeaderBytes = 64u;
constexpr uint32_t kConstantsBytes = 192u;
constexpr uint32_t kPageHeaderBytes = kHeaderBytes + kConstantsBytes;
constexpr uint32_t kMaxShDegree = 3u;
constexpr uint32_t kDigestBytes = 32u;

// Mirrors gs_world_writer::CodecParams; the defaults are the D-12 pre-registered values.
struct EncodeParams {
	uint8_t sh_bits[3] = { 6, 5, 5 }; // 2..8
	uint8_t opacity_bits = 16; // 8..16
	float pos_step_m = 0.00025f; // Positions take 21 bits when 16 would be coarser than this.
	int zstd_level = 3; // 1..22
};

struct PageInfo {
	uint32_t splat_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_high_order = 0; // Vector3 coefficients per splat in the high-order array
	uint32_t position_bits = 0; // 16 or 21
	bool has_normals = false;
	uint32_t stored_bytes = 0;
	uint32_t sub_block_count = 0;
	uint32_t frame_bytes[kMaxSubBlocks] = {};
	uint8_t sh_bits[3] = {};
	uint8_t opacity_bits = 0;
	float position_min[3] = {};
	float position_max[3] = {};
	int32_t log_scale_min[3] = {};
	int32_t log_scale_max[3] = {};
	float dc_min[3] = {};
	float dc_max[3] = {};
	float sh_max[3] = {};
	Gaussian constants; // normal (when not per-splat), area, sh_dc.a, stroke_age, brush_axes, metas
	uint8_t stored_digest[kDigestBytes] = {};
};

// In Vector3 units: 0 up to degree 1, then 5 for degree 2 and 12 for degree 3.
constexpr uint32_t sh_high_order_for_degree(uint32_t p_degree) {
	return p_degree < 2u ? 0u : (p_degree + 1u) * (p_degree + 1u) - 4u;
}

// Encodes p_count splats. p_sh_high holds sh_high_order_for_degree() Vector3 per splat and may
// be null when that is 0. r_page holds exactly the stored bytes on OK and is empty otherwise.
// The same input and zstd build produce the same bytes.
Error encode_page(const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count, uint32_t p_sh_degree,
		const EncodeParams &p_params, LocalVector<uint8_t> &r_page, String &r_reason);

// Structural validation of the 256 B header; touches no frame and checks no digest.
Error parse_page_header(const uint8_t *p_page, uint64_t p_page_bytes, PageInfo &r_info, String &r_reason);

// parse_page_header() plus the stored SHA-256.
Error verify_page(const uint8_t *p_page, uint64_t p_page_bytes, PageInfo &r_info, String &r_reason);

// SHA-256 of the canonical little-endian decoded fields: every Gaussian field except padding,
// then the high-order SH. It covers the floats the decoder produced, so it pins dequantization
// as well as the quantized integers.
void decoded_digest(const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count, uint32_t p_sh_degree,
		uint8_t r_digest[kDigestBytes]);

// The zstd version the module was compiled against and the one it links (risk K11).
uint32_t zstd_header_version();
uint32_t zstd_library_version();

// Owns one ZSTD_DCtx and the scratch of one decoding thread: one instance per worker, never
// shared. Godot's Compression::decompress serialises every caller on one mutex (D-12).
class DecodeContext {
	ZSTD_DCtx_s *dctx = nullptr;
	LocalVector<uint8_t> raw;
	float sh_lut[3][256];
	float scale_lut[3][1024];

public:
	DecodeContext();
	~DecodeContext();
	DecodeContext(const DecodeContext &) = delete;
	DecodeContext &operator=(const DecodeContext &) = delete;

	// Decodes splats [p_first, p_first + p_count) into r_gaussians[0..p_count) and, with
	// sh_high_order Vector3 per splat, r_sh_high (may be null when that is 0). Only the
	// sub-blocks in the range are decompressed. p_verify_digest checks the page SHA-256 first,
	// which needs all of the page's stored bytes; without it each frame still carries its zstd
	// content checksum. The outputs are undefined on failure.
	Error decode_range(const uint8_t *p_page, uint64_t p_page_bytes, uint32_t p_first, uint32_t p_count,
			Gaussian *r_gaussians, Vector3 *r_sh_high, String &r_reason, bool p_verify_digest = true);
};

} // namespace gs_gsq
