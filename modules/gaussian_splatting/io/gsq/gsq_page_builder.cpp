#include "gsq_page_builder.h"

#include <zstd.h>

#include <cmath>
#include <cstring>

namespace gs_gsq {

#ifdef TESTS_ENABLED
PageBuilder::PageTestHook PageBuilder::post_encode_test_hook = nullptr;
#endif

namespace {

constexpr uint32_t kKeyWords = 7u;
constexpr uint32_t kMortonAxisBits = 21u;

inline uint32_t bits_of(float p_value) {
	uint32_t bits = 0u;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

inline bool is_finite(float p_value) {
	return (bits_of(p_value) & 0x7F800000u) != 0x7F800000u;
}

// The codec's page constants (constants_match() in gsq_codec.cpp), compared by bit pattern.
// The normal is not one: the codec stores per-splat normals when they differ.
void constant_key(const Gaussian &p_gaussian, uint32_t r_key[kKeyWords]) {
	r_key[0] = bits_of(p_gaussian.area);
	r_key[1] = bits_of(p_gaussian.sh_dc.a);
	r_key[2] = bits_of(p_gaussian.stroke_age);
	r_key[3] = bits_of(p_gaussian.brush_axes.x);
	r_key[4] = bits_of(p_gaussian.brush_axes.y);
	r_key[5] = p_gaussian.painterly_meta;
	r_key[6] = p_gaussian.render_meta;
}

struct GroupEntry {
	uint32_t key[kKeyWords];
	uint32_t index;

	bool same_group(const GroupEntry &p_other) const {
		return memcmp(key, p_other.key, sizeof(key)) == 0;
	}
	// Groups by key, importance order (index) inside a group.
	bool operator<(const GroupEntry &p_other) const {
		for (uint32_t k = 0u; k < kKeyWords; k++) {
			if (key[k] != p_other.key[k]) {
				return key[k] < p_other.key[k];
			}
		}
		return index < p_other.index;
	}
};

struct PageSpan {
	uint32_t first_source; // importance rank of the page's first splat; unique per page
	uint32_t begin; // into the grouped entries
	uint32_t count;
	uint32_t group; // run number in key order

	bool operator<(const PageSpan &p_other) const {
		return first_source < p_other.first_source;
	}
};

struct MortonEntry {
	uint64_t key;
	uint32_t source;

	bool operator<(const MortonEntry &p_other) const {
		return key != p_other.key ? key < p_other.key : source < p_other.source;
	}
};

// Order-preserving map of a float's bits onto u32 (negative values reversed below positive).
inline uint32_t ordered_bits(float p_value) {
	const uint32_t bits = bits_of(p_value);
	return (bits >> 31u) != 0u ? ~bits : (bits | 0x80000000u);
}

inline uint32_t bit_length(uint32_t p_value) {
	uint32_t length = 0u;
	while (p_value != 0u) {
		length++;
		p_value >>= 1u;
	}
	return length;
}

// The low 21 bits of p_value spread to every third bit.
inline uint64_t spread3(uint32_t p_value) {
	uint64_t x = p_value & 0x1FFFFFu;
	x = (x | (x << 32u)) & 0x1F00000000FFFFull;
	x = (x | (x << 16u)) & 0x1F0000FF0000FFull;
	x = (x | (x << 8u)) & 0x100F00F00F00F00Full;
	x = (x | (x << 4u)) & 0x10C30C30C30C30C3ull;
	x = (x | (x << 2u)) & 0x1249249249249249ull;
	return x;
}

inline float step_tolerance(float p_lo, float p_hi, float p_steps) {
	// 0.6 of a quantization step (the codec is within 0.5), plus float rounding at this magnitude.
	const float magnitude = MAX(1.0f, MAX(std::fabs(p_lo), std::fabs(p_hi)));
	return (p_hi - p_lo) / p_steps * 0.6f + magnitude * 1e-6f;
}

} // namespace

Error validate_encode_params(const EncodeParams &p_params, String &r_reason) {
	for (uint8_t bits : p_params.sh_bits) {
		if (bits < 2u || bits > 8u) {
			r_reason = vformat("gsq_invalid_params: sh_bits %d outside [2, 8]", bits);
			return ERR_INVALID_PARAMETER;
		}
	}
	if (p_params.opacity_bits < 8u || p_params.opacity_bits > 16u) {
		r_reason = vformat("gsq_invalid_params: opacity_bits %d outside [8, 16]", p_params.opacity_bits);
		return ERR_INVALID_PARAMETER;
	}
	if (!is_finite(p_params.pos_step_m) || !(p_params.pos_step_m > 0.0f)) {
		r_reason = "gsq_invalid_params: pos_step_m must be finite and positive";
		return ERR_INVALID_PARAMETER;
	}
	if (p_params.zstd_level < 1 || p_params.zstd_level > ZSTD_maxCLevel()) {
		r_reason = vformat("gsq_invalid_params: zstd_level %d outside [1, %d]", p_params.zstd_level, ZSTD_maxCLevel());
		return ERR_INVALID_PARAMETER;
	}
	return OK;
}

Error PageBuilder::configure(const EncodeParams &p_params, uint32_t p_sh_degree, String &r_reason) {
	configured = false;
	if (p_sh_degree > kMaxShDegree) {
		r_reason = vformat("gsq_sh_degree_unsupported: degree %d", p_sh_degree);
		return ERR_INVALID_PARAMETER;
	}
	const Error err = validate_encode_params(p_params, r_reason);
	if (err != OK) {
		return err;
	}
	params = p_params;
	sh_degree = p_sh_degree;
	sh_high_order = sh_high_order_for_degree(p_sh_degree);
	stats = PageBuildStats();
	configured = true;
	return OK;
}

Error PageBuilder::build_chunk(uint32_t p_chunk_index, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count,
		LocalVector<BuiltPage> &r_pages, String &r_reason) {
	if (!configured) {
		r_reason = "gsq_builder_unconfigured";
		return ERR_UNCONFIGURED;
	}
	if (p_count == 0u) {
		stats.chunks++;
		return OK;
	}
	if (p_gaussians == nullptr || (sh_high_order > 0u && p_sh_high == nullptr)) {
		r_reason = vformat("chunk %d: gsq_sh_layout: gaussians or high-order SH missing", p_chunk_index);
		return ERR_INVALID_PARAMETER;
	}

	// 1. Constant groups, importance order inside each.
	LocalVector<GroupEntry> entries;
	entries.resize(p_count);
	for (uint32_t i = 0u; i < p_count; i++) {
		constant_key(p_gaussians[i], entries[i].key);
		entries[i].index = i;
	}
	entries.sort();

	// 2. Each group cut into importance-ordered pages; the chunk's pages ordered by first splat.
	LocalVector<PageSpan> spans;
	uint32_t group_count = 0u;
	for (uint32_t run = 0u; run < p_count;) {
		uint32_t end = run + 1u;
		while (end < p_count && entries[end].same_group(entries[run])) {
			end++;
		}
		for (uint32_t begin = run; begin < end; begin += kWriterPageSplats) {
			spans.push_back(PageSpan{ entries[begin].index, begin, MIN(kWriterPageSplats, end - begin), group_count });
		}
		group_count++;
		run = end;
	}
	spans.sort();
	LocalVector<uint32_t> group_ordinal;
	group_ordinal.resize(group_count);
	for (uint32_t &ordinal : group_ordinal) {
		ordinal = UINT32_MAX;
	}
	uint32_t next_ordinal = 0u;

	const uint32_t first_page = r_pages.size();
	LocalVector<MortonEntry> order;
	for (uint32_t p = 0u; p < spans.size(); p++) {
		const PageSpan &span = spans[p];
		if (group_ordinal[span.group] == UINT32_MAX) {
			group_ordinal[span.group] = next_ordinal++;
		}

		// 3. Morton order inside the page.
		uint32_t lo[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
		uint32_t hi[3] = { 0u, 0u, 0u };
		for (uint32_t j = 0u; j < span.count; j++) {
			const Vector3 &position = p_gaussians[entries[span.begin + j].index].position;
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				const uint32_t o = ordered_bits(position[axis]);
				lo[axis] = MIN(lo[axis], o);
				hi[axis] = MAX(hi[axis], o);
			}
		}
		uint32_t shift[3];
		for (uint32_t axis = 0u; axis < 3u; axis++) {
			const uint32_t length = bit_length(hi[axis] - lo[axis]);
			shift[axis] = length > kMortonAxisBits ? length - kMortonAxisBits : 0u;
		}
		order.resize(span.count);
		for (uint32_t j = 0u; j < span.count; j++) {
			const uint32_t source = entries[span.begin + j].index;
			const Vector3 &position = p_gaussians[source].position;
			uint64_t key = 0u;
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				key |= spread3((ordered_bits(position[axis]) - lo[axis]) >> shift[axis]) << axis;
			}
			order[j] = MortonEntry{ key, source };
		}
		order.sort();

		BuiltPage page;
		page.source_index.resize(span.count);
		page_gaussians.resize(span.count);
		page_sh.resize(span.count * sh_high_order);
		for (uint32_t j = 0u; j < span.count; j++) {
			const uint32_t source = order[j].source;
			page.source_index[j] = source;
			page_gaussians[j] = p_gaussians[source];
			for (uint32_t k = 0u; k < sh_high_order; k++) {
				page_sh[j * sh_high_order + k] = p_sh_high[uint64_t(source) * sh_high_order + k];
			}
		}
		const Vector3 *sh = sh_high_order > 0u ? page_sh.ptr() : nullptr;

		String reason;
		Error err = encode_page(page_gaussians.ptr(), sh, span.count, sh_degree, params, page.bytes, reason);
#ifdef TESTS_ENABLED
		if (err == OK && post_encode_test_hook != nullptr) {
			post_encode_test_hook(page.bytes);
		}
#endif
		PageRecord &record = page.record;
		record.chunk_index = p_chunk_index;
		record.page_in_chunk = p;
		record.constant_group = group_ordinal[span.group];
		record.splat_count = span.count;
		record.first_source = span.first_source;
		record.last_source = entries[span.begin + span.count - 1u].index;
		if (err == OK) {
			// 4. W12.
			err = _validate_page(page.bytes.ptr(), page.bytes.size(), page_gaussians.ptr(), sh, span.count, record, reason);
		}
		if (err != OK) {
			r_pages.resize(first_page);
			r_reason = vformat("chunk %d page %d (%d splats from importance rank %d): %s", p_chunk_index, p, span.count,
					span.first_source, reason);
			return err;
		}
		r_pages.push_back(page);
	}

	uint64_t stored = 0u;
	for (uint32_t p = first_page; p < r_pages.size(); p++) {
		stored += r_pages[p].record.stored_bytes;
	}
	stats.chunks++;
	stats.splats += p_count;
	stats.pages += spans.size();
	stats.stored_bytes += stored;
	stats.constant_groups += group_count;
	if (group_count > 1u) {
		stats.heterogeneous_chunks++;
	}
	return OK;
}

Error PageBuilder::_validate_page(const uint8_t *p_page, uint32_t p_page_bytes, const Gaussian *p_source, const Vector3 *p_source_sh,
		uint32_t p_count, PageRecord &r_record, String &r_reason) {
	PageInfo info;
	String reason;
	Error err = verify_page(p_page, p_page_bytes, info, reason);
	if (err != OK) {
		r_reason = "w12_verify: " + reason;
		return err;
	}
	if (info.splat_count != p_count || info.sh_degree != sh_degree || info.stored_bytes != p_page_bytes) {
		r_reason = vformat("w12_header: %d splats of degree %d in %d bytes, expected %d of degree %d in %d", info.splat_count,
				info.sh_degree, info.stored_bytes, p_count, sh_degree, p_page_bytes);
		return ERR_INVALID_DATA;
	}
	decoded_gaussians.resize(p_count);
	decoded_sh.resize(p_count * sh_high_order);
	Vector3 *decoded_high = sh_high_order > 0u ? decoded_sh.ptr() : nullptr;
	// The stored digest was verified above.
	err = decoder.decode_range(p_page, p_page_bytes, 0u, p_count, decoded_gaussians.ptr(), decoded_high, reason, false);
	if (err != OK) {
		r_reason = "w12_decode: " + reason;
		return err;
	}

	float position_tol[3];
	float dc_tol[3];
	float scale_tol[3];
	float sh_tol[3];
	constexpr float kDcSteps[3] = { 2047.0f, 2047.0f, 1023.0f };
	for (uint32_t axis = 0u; axis < 3u; axis++) {
		position_tol[axis] = step_tolerance(info.position_min[axis], info.position_max[axis], float((1u << info.position_bits) - 1u));
		dc_tol[axis] = step_tolerance(info.dc_min[axis], info.dc_max[axis], kDcSteps[axis]);
		scale_tol[axis] = float(info.log_scale_max[axis] - info.log_scale_min[axis]) / 65536.0f / 1023.0f * 0.6f + 1e-4f;
		sh_tol[axis] = info.sh_max[axis] / float((1u << (info.sh_bits[axis] - 1u)) - 1u) * 0.6f + 1e-6f;
	}
	const float opacity_tol = 0.6f / float((1u << info.opacity_bits) - 1u) + 1e-6f;
	const float normal_tol = 0.6f / 32767.0f + 1e-6f;

	for (uint32_t i = 0u; i < p_count; i++) {
		const Gaussian &s = p_source[i];
		const Gaussian &d = decoded_gaussians[i];
		const char *field = nullptr;
		const float decoded_floats[] = { d.position.x, d.position.y, d.position.z, d.opacity, d.scale.x, d.scale.y, d.scale.z,
			d.rotation.x, d.rotation.y, d.rotation.z, d.rotation.w, d.sh_dc.r, d.sh_dc.g, d.sh_dc.b, d.normal.x, d.normal.y, d.normal.z };
		for (float value : decoded_floats) {
			if (!is_finite(value)) {
				field = "non-finite decoded field";
			}
		}
		if (bits_of(d.area) != bits_of(s.area) || bits_of(d.sh_dc.a) != bits_of(s.sh_dc.a) ||
				bits_of(d.stroke_age) != bits_of(s.stroke_age) || bits_of(d.brush_axes.x) != bits_of(s.brush_axes.x) ||
				bits_of(d.brush_axes.y) != bits_of(s.brush_axes.y) || d.painterly_meta != s.painterly_meta ||
				d.render_meta != s.render_meta) {
			field = "page constant";
		}
		for (uint32_t axis = 0u; axis < 3u && field == nullptr; axis++) {
			if (!(std::fabs(d.position[axis] - s.position[axis]) <= position_tol[axis])) {
				field = "position";
			} else if (!(std::fabs(d.sh_dc[axis] - s.sh_dc[axis]) <= dc_tol[axis])) {
				field = "DC";
			} else if (info.has_normals ? !(std::fabs(d.normal[axis] - CLAMP(s.normal[axis], -1.0f, 1.0f)) <= normal_tol)
										: bits_of(d.normal[axis]) != bits_of(s.normal[axis])) {
				field = "normal";
			} else {
				const float source_scale = std::fabs(s.scale[axis]);
				if (!(d.scale[axis] > 0.0f)) {
					field = "scale";
				} else if (source_scale < 0x1p-31f ? d.scale[axis] > 0x1p-30f
												   : !(std::fabs(std::log2(d.scale[axis]) - std::log2(source_scale)) <= scale_tol[axis])) {
					field = "scale";
				}
			}
		}
		if (field == nullptr && !(std::fabs(d.opacity - s.opacity) <= opacity_tol)) {
			field = "opacity";
		}
		if (field == nullptr) {
			const float length = std::sqrt(s.rotation.dot(s.rotation));
			if (!(length > 0.0f) || !(std::fabs(s.rotation.dot(d.rotation)) / length >= 0.9999f)) {
				field = "rotation";
			}
		}
		for (uint32_t v = 0u; v < 3u && field == nullptr; v++) {
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				if (!(std::fabs(d.sh_1[v][axis] - s.sh_1[v][axis]) <= sh_tol[0])) {
					field = "first-order SH";
				}
			}
		}
		for (uint32_t k = 0u; k < sh_high_order && field == nullptr; k++) {
			const Vector3 &a = p_source_sh[uint64_t(i) * sh_high_order + k];
			const Vector3 &b = decoded_sh[i * sh_high_order + k];
			const float tol = sh_tol[k < 5u ? 1u : 2u];
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				if (!(std::fabs(a[axis] - b[axis]) <= tol)) {
					field = "high-order SH";
				}
			}
		}
		if (field != nullptr) {
			r_reason = vformat("w12_mismatch: splat %d %s outside its D-12 step after the round trip", i, field);
			return ERR_INVALID_DATA;
		}
	}

	// 5. W15.
	r_record.stored_bytes = info.stored_bytes;
	memcpy(r_record.stored_digest, info.stored_digest, kDigestBytes);
	decoded_digest(decoded_gaussians.ptr(), decoded_high, p_count, sh_degree, r_record.decoded_digest);
	return OK;
}

} // namespace gs_gsq
