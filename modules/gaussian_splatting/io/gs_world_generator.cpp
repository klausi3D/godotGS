#include "gs_world_generator.h"

#include "gaussian_splat_world_format.h"
#include "gs_atomic_file_transaction.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"

#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace gs_world_generator {

namespace {

// ---------------------------------------------------------------------------
// Integer hashing and exact arithmetic helpers.
// ---------------------------------------------------------------------------

constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
// R2 additive recurrence (plastic constant), 32-bit fixed point.
constexpr uint32_t kR2A1 = 3242174889u;
constexpr uint32_t kR2A2 = 2447445414u;
// Fixed-point position unit: 2^-24 m.
constexpr int kFpBits = 24;
constexpr int64_t kFpOne = int64_t(1) << kFpBits;
// 1/4096 m ("q") to fixed point.
constexpr int kQToFpShift = kFpBits - 12;
constexpr int kMaxPlacementAttempts = 100000;
constexpr uint32_t kSplatsPerTask = 1024u;
constexpr uint64_t kReadSlice = uint64_t(4) << 20u;
constexpr int64_t kNoiseOffset = int64_t(1) << 40;
constexpr int kLatents = 4;

// Hash stream domains.
enum : uint64_t {
	DOM_PLACE = 0x51,
	DOM_PART = 0x52,
	DOM_SPLAT = 0x53,
	DOM_BUILDING = 0x54,
	DOM_NOISE = 0x55,
	DOM_SH_WEIGHT = 0x56,
	DOM_BEACON = 0x57,
};

inline uint64_t mix64(uint64_t z) {
	z += kGolden;
	z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31u);
}

inline uint64_t hash4(uint64_t p_a, uint64_t p_b, uint64_t p_c, uint64_t p_d) {
	return mix64(mix64(mix64(mix64(p_a) ^ p_b) ^ p_c) ^ p_d);
}

inline uint64_t hash5(uint64_t p_a, uint64_t p_b, uint64_t p_c, uint64_t p_d, uint64_t p_e) {
	return mix64(hash4(p_a, p_b, p_c, p_d) ^ p_e);
}

// floor(sqrt(v)), exact.
uint64_t isqrt_floor(uint64_t p_v) {
	uint64_t res = 0;
	uint64_t bit = uint64_t(1) << 62u;
	while (bit > p_v) {
		bit >>= 2u;
	}
	while (bit != 0u) {
		if (p_v >= res + bit) {
			p_v -= res + bit;
			res = (res >> 1u) + bit;
		} else {
			res >>= 1u;
		}
		bit >>= 2u;
	}
	return res;
}

uint64_t isqrt_ceil(uint64_t p_v) {
	const uint64_t f = isqrt_floor(p_v);
	return f * f == p_v ? f : f + 1u;
}

// Fixed point (2^-24 m) to float: exact in double, then rounded toward negative
// infinity. Round-to-nearest could lift a coordinate just below a cell's upper edge onto
// the edge (the float grid is coarser than 2^-24 m away from the origin), putting the
// splat in the next chunk's cell; rounding down keeps x0 <= x < x1 for every
// representable integer edge x0. Both the comparison and nextafter are exact.
inline float fp_to_float(int64_t p_fp) {
	const double d = double(p_fp) * (1.0 / double(kFpOne));
	float f = float(d);
	if (double(f) > d) {
		f = std::nextafter(f, -INFINITY);
	}
	return f;
}

inline float q12_to_float(int64_t p_q) {
	return float(double(p_q) * (1.0 / 4096.0));
}

uint64_t morton2(uint32_t p_x, uint32_t p_z) {
	auto spread = [](uint64_t v) {
		v &= 0xFFFFFFFFull;
		v = (v | (v << 16u)) & 0x0000FFFF0000FFFFull;
		v = (v | (v << 8u)) & 0x00FF00FF00FF00FFull;
		v = (v | (v << 4u)) & 0x0F0F0F0F0F0F0F0Full;
		v = (v | (v << 2u)) & 0x3333333333333333ull;
		v = (v | (v << 1u)) & 0x5555555555555555ull;
		return v;
	};
	return spread(p_x) | (spread(p_z) << 1u);
}

uint32_t bit_reverse(uint32_t p_v, uint32_t p_bits) {
	uint32_t r = 0;
	for (uint32_t i = 0; i < p_bits; i++) {
		r = (r << 1u) | ((p_v >> i) & 1u);
	}
	return r;
}

bool is_pow2(int64_t p_v) {
	return p_v > 0 && (p_v & (p_v - 1)) == 0;
}

uint32_t log2_exact(int64_t p_v) {
	uint32_t r = 0;
	while ((int64_t(1) << r) < p_v) {
		r++;
	}
	return r;
}

String sha_hex(const unsigned char p_hash[32]) {
	return String::hex_encode_buffer(p_hash, 32);
}

String sha256_of_string(const String &p_text) {
	const CharString utf8 = p_text.utf8();
	CryptoCore::SHA256Context ctx;
	ctx.start();
	ctx.update(reinterpret_cast<const uint8_t *>(utf8.get_data()), size_t(utf8.length()));
	unsigned char h[32];
	ctx.finish(h);
	return sha_hex(h);
}

// CRC-16/CCITT-FALSE.
uint16_t crc16(uint16_t p_value) {
	uint16_t crc = 0xFFFFu;
	const uint8_t bytes[2] = { uint8_t(p_value >> 8u), uint8_t(p_value & 0xFFu) };
	for (uint8_t b : bytes) {
		crc ^= uint16_t(b) << 8u;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 0x8000u) ? uint16_t((crc << 1u) ^ 0x1021u) : uint16_t(crc << 1u);
		}
	}
	return crc;
}

// 8 x 8 code cells, bit (row * 8 + col), row 0 at the panel bottom. Rows 0 and 7 are
// finder rows (asymmetric under X- and Y-flip); rows 1-2 the id, 3-4 its complement,
// 5-6 its CRC-16.
uint64_t beacon_code(uint16_t p_id) {
	const uint64_t finder_bottom = 0x4Full; // 0b01001111
	const uint64_t finder_top = 0xF2ull; // 0b11110010
	uint64_t code = finder_bottom;
	code |= uint64_t(p_id) << 8u;
	code |= uint64_t(uint16_t(~p_id)) << 24u;
	code |= uint64_t(crc16(p_id)) << 40u;
	code |= finder_top << 56u;
	return code;
}

// ---------------------------------------------------------------------------
// Canonical JSON.
// ---------------------------------------------------------------------------

void json_escape(const String &p_s, String &r_out) {
	r_out += "\"";
	for (int i = 0; i < p_s.length(); i++) {
		const char32_t c = p_s[i];
		if (c == '"') {
			r_out += "\\\"";
		} else if (c == '\\') {
			r_out += "\\\\";
		} else if (c < 0x20 || c > 0x7E) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c & 0xFFFFu));
			r_out += buf;
		} else {
			r_out += String::chr(c);
		}
	}
	r_out += "\"";
}

void json_emit(const Variant &p_v, String &r_out) {
	switch (p_v.get_type()) {
		case Variant::NIL:
			r_out += "null";
			break;
		case Variant::BOOL:
			r_out += bool(p_v) ? "true" : "false";
			break;
		case Variant::INT:
			r_out += itos(int64_t(p_v));
			break;
		case Variant::FLOAT: {
			const double d = double(p_v);
			if (d == double(int64_t(d)) && d > -9007199254740992.0 && d < 9007199254740992.0) {
				r_out += itos(int64_t(d));
			} else {
				// Only float32 values reach the manifest; 9 significant digits round-trip
				// them, and printf rounds correctly on every supported CRT.
				char buf[40];
				snprintf(buf, sizeof(buf), "%.9g", d);
				r_out += buf;
			}
		} break;
		case Variant::STRING:
		case Variant::STRING_NAME:
			json_escape(String(p_v), r_out);
			break;
		case Variant::ARRAY: {
			const Array a = p_v;
			r_out += "[";
			for (int i = 0; i < a.size(); i++) {
				if (i > 0) {
					r_out += ",";
				}
				json_emit(a[i], r_out);
			}
			r_out += "]";
		} break;
		case Variant::DICTIONARY: {
			const Dictionary d = p_v;
			LocalVector<String> keys;
			for (const Variant &k : d.keys()) {
				keys.push_back(String(k));
			}
			// Byte-wise (ASCII) order, which is Python's sort for ASCII keys.
			std::sort(keys.ptr(), keys.ptr() + keys.size(), [](const String &a, const String &b) {
				return strcmp(a.ascii().get_data(), b.ascii().get_data()) < 0;
			});
			r_out += "{";
			for (uint32_t i = 0; i < keys.size(); i++) {
				if (i > 0) {
					r_out += ",";
				}
				json_escape(keys[i], r_out);
				r_out += ":";
				json_emit(d[keys[i]], r_out);
			}
			r_out += "}";
		} break;
		default:
			// Not produced by this module; make it visible rather than silent.
			r_out += "\"<unsupported>\"";
			break;
	}
}

// ---------------------------------------------------------------------------
// Parameter dictionary mapping.
// ---------------------------------------------------------------------------

struct IntField {
	const char *group; // nullptr = top level
	const char *key;
	int64_t Params::*member;
};

const IntField kIntFields[] = {
	{ nullptr, "seed", &Params::seed },
	{ "world", "tile_size_m", &Params::tile_size_m },
	{ "world", "tiles_x", &Params::tiles_x },
	{ "world", "tiles_z", &Params::tiles_z },
	{ "world", "base_cell_m", &Params::base_cell_m },
	{ "world", "min_cell_m", &Params::min_cell_m },
	{ "world", "max_chunk_splats", &Params::max_chunk_splats },
	{ "world", "total_splats", &Params::total_splats },
	{ "placement", "margin_m", &Params::margin_m },
	{ "placement", "edge_margin_m", &Params::edge_margin_m },
	{ "plain", "y_jitter_q", &Params::plain_y_jitter_q },
	{ "plain", "ground_flatten_shift", &Params::ground_flatten_shift },
	{ "town", "count", &Params::town_count },
	{ "town", "size_m", &Params::town_size_m },
	{ "town", "block_m", &Params::town_block_m },
	{ "town", "building_m", &Params::town_building_m },
	{ "town", "height_min_m", &Params::town_height_min_m },
	{ "town", "height_max_m", &Params::town_height_max_m },
	{ "town", "density_per_m2", &Params::town_density_per_m2 },
	{ "town", "roof_permille", &Params::town_roof_permille },
	{ "wall", "length_m", &Params::wall_length_m },
	{ "wall", "height_m", &Params::wall_height_m },
	{ "wall", "footprint_m", &Params::wall_footprint_m },
	{ "wall", "separation_m", &Params::wall_separation_m },
	{ "wall", "density_per_m2", &Params::wall_density_per_m2 },
	{ "wall", "corridor_z_m", &Params::wall_corridor_z_m },
	{ "foliage", "count", &Params::foliage_count },
	{ "foliage", "size_m", &Params::foliage_size_m },
	{ "foliage", "density_per_m2", &Params::foliage_density_per_m2 },
	{ "foliage", "canopy_min_q", &Params::foliage_canopy_min_q },
	{ "foliage", "canopy_max_q", &Params::foliage_canopy_max_q },
	{ "beacon", "grid", &Params::beacon_grid },
	{ "beacon", "panel_m", &Params::beacon_panel_m },
	{ "beacon", "base_y_q", &Params::beacon_base_y_q },
	{ "sh", "degree", &Params::sh_degree },
	{ "sh", "corr_log2_m", &Params::sh_corr_log2_m },
	{ "sh", "octaves", &Params::sh_octaves },
	{ "sh", "field_amp_q8", &Params::sh_field_amp_q8 },
	{ "sh", "jitter_amp", &Params::sh_jitter_amp },
	{ "sh", "band_shift_1", &Params::sh_band_shift_1 },
	{ "sh", "band_shift_2", &Params::sh_band_shift_2 },
	{ "sh", "band_shift_3", &Params::sh_band_shift_3 },
};

const char *kClassGroup[CLASS_COUNT] = { "plain", "town", "wall", "foliage", "beacon" };

struct StyleField {
	const char *key;
	int64_t ClassStyle::*member;
};

const StyleField kStyleFields[] = {
	{ "sigma_min_q", &ClassStyle::sigma_min_q },
	{ "sigma_max_q", &ClassStyle::sigma_max_q },
	{ "opacity_min_q8", &ClassStyle::opacity_min_q8 },
	{ "opacity_max_q8", &ClassStyle::opacity_max_q8 },
	{ "color_r", &ClassStyle::color_r },
	{ "color_g", &ClassStyle::color_g },
	{ "color_b", &ClassStyle::color_b },
	{ "color_var_q8", &ClassStyle::color_var_q8 },
};

bool variant_to_int(const Variant &p_v, int64_t &r_out) {
	if (p_v.get_type() == Variant::INT) {
		r_out = int64_t(p_v);
		return true;
	}
	if (p_v.get_type() == Variant::FLOAT) {
		const double d = double(p_v);
		if (d == double(int64_t(d)) && d > -9007199254740992.0 && d < 9007199254740992.0) {
			r_out = int64_t(d);
			return true;
		}
	}
	return false;
}

Dictionary &group_of(const char *p_group, HashMap<String, Dictionary> &r_groups) {
	if (!r_groups.has(p_group)) {
		r_groups[p_group] = Dictionary();
	}
	return r_groups[p_group];
}

void style_defaults(Params &r) {
	// sigma 87..175 q (2.1-4.3 cm) = 2 sigma of 3-6 px at 10 m (1920 px over a 107.5 deg
	// horizontal FOV, 1 px = 1.42 cm at 10 m; ADR §6.2 "3-6 px at 10 m").
	r.style[CLASS_PLAIN] = ClassStyle{ 87, 175, 160, 250, 96, 128, 64, 48 };
	r.style[CLASS_TOWN] = ClassStyle{ 87, 175, 200, 255, 150, 140, 130, 64 };
	r.style[CLASS_WALL] = ClassStyle{ 87, 175, 220, 255, 170, 170, 165, 32 };
	// Foliage: the low-alpha translucent class (#1290 failure mode), opacity 0.05-0.25.
	r.style[CLASS_FOLIAGE] = ClassStyle{ 175, 350, 13, 64, 60, 140, 50, 64 };
	// Beacons: opaque coded panels; a code bit 1 takes the style colour, 0 its complement.
	r.style[CLASS_BEACON] = ClassStyle{ 256, 256, 255, 255, 240, 240, 240, 0 };
}

void beacon_defaults(Params &r) {
	r.beacon_cells.clear();
	const int64_t cells[8] = { 2, 2, 5, 2, 2, 5, 5, 5 };
	for (int64_t c : cells) {
		r.beacon_cells.push_back(c);
	}
}

} // namespace

const char *class_name(ContentClass p_class) {
	switch (p_class) {
		case CLASS_PLAIN:
			return "plain";
		case CLASS_TOWN:
			return "town";
		case CLASS_WALL:
			return "wall";
		case CLASS_FOLIAGE:
			return "foliage";
		case CLASS_BEACON:
			return "beacon";
		default:
			return "unknown";
	}
}

IRect IRect::intersection(const IRect &p_o) const {
	IRect r{ MAX(x0, p_o.x0), MAX(z0, p_o.z0), MIN(x1, p_o.x1), MIN(z1, p_o.z1) };
	if (r.x1 < r.x0) {
		r.x1 = r.x0;
	}
	if (r.z1 < r.z0) {
		r.z1 = r.z0;
	}
	return r;
}

PackedStringArray get_tier_names() {
	PackedStringArray names;
	names.push_back("T0");
	names.push_back("A");
	names.push_back("A-mini");
	names.push_back("T-smoke");
	return names;
}

Error params_for_tier(const String &p_tier, Params &r_params) {
	Params p;
	style_defaults(p);
	beacon_defaults(p);
	p.tier = p_tier;
	if (p_tier == "A") {
		// ADR §6.2: the exact Tier A content table.
		p.seed = 0xB505A0;
		p.tiles_x = 8;
		p.tiles_z = 8;
		p.total_splats = 201326592;
		p.town_count = 6;
		p.wall_length_m = 1800;
		p.foliage_count = 64;
	} else if (p_tier == "T0") {
		// ADR §6.1: 4,194,304 splats on 512 x 512 m. Every Tier A class is present; the
		// plain is the remainder, so its density is lower than A's.
		p.seed = 0xB50570;
		p.tiles_x = 2;
		p.tiles_z = 2;
		p.total_splats = 4194304;
		p.town_count = 1;
		p.wall_length_m = 400;
		p.foliage_count = 4;
	} else if (p_tier == "A-mini") {
		// Tier A's densities (plain included: floor(252,144 m2 x 176,758,016 / 4,105,504))
		// on 2 x 2 tiles: the scaled Tier A for local evidence.
		p.seed = 0xB505A1;
		p.tiles_x = 2;
		p.tiles_z = 2;
		p.total_splats = 13881322;
		p.town_count = 1;
		p.wall_length_m = 400;
		p.foliage_count = 4;
	} else if (p_tier == "T-smoke") {
		// One tile with reduced densities for unit tests (262,144 splats).
		p.seed = 0xB50551;
		p.tiles_x = 1;
		p.tiles_z = 1;
		p.total_splats = 262144;
		p.town_count = 1;
		p.town_density_per_m2 = 25;
		p.wall_length_m = 200;
		p.wall_density_per_m2 = 20;
		p.foliage_count = 2;
		p.foliage_density_per_m2 = 20;
		p.beacon_grid = 32;
	} else {
		return ERR_INVALID_PARAMETER;
	}
	r_params = p;
	return OK;
}

Dictionary params_to_dict(const Params &p) {
	HashMap<String, Dictionary> groups;
	Dictionary root;
	root["schema"] = kParamsSchema;
	root["tier"] = p.tier;
	for (const IntField &f : kIntFields) {
		if (f.group == nullptr) {
			root[f.key] = p.*(f.member);
		} else {
			Dictionary &g = group_of(f.group, groups);
			g[f.key] = p.*(f.member);
		}
	}
	for (int c = 0; c < CLASS_COUNT; c++) {
		Dictionary &g = group_of(kClassGroup[c], groups);
		Dictionary style;
		for (const StyleField &s : kStyleFields) {
			style[s.key] = p.style[c].*(s.member);
		}
		g["style"] = style;
	}
	Array cells;
	for (int64_t v : p.beacon_cells) {
		cells.push_back(v);
	}
	group_of("beacon", groups)["cells"] = cells;
	for (const KeyValue<String, Dictionary> &kv : groups) {
		root[kv.key] = kv.value;
	}
	return root;
}

namespace {

Error check_keys(const Dictionary &p_dict, const PackedStringArray &p_expected, const String &p_where, String &r_error) {
	for (const Variant &k : p_dict.keys()) {
		if (!p_expected.has(String(k))) {
			r_error = vformat("unknown parameter '%s%s'", p_where, String(k));
			return ERR_INVALID_PARAMETER;
		}
	}
	for (const String &k : p_expected) {
		if (!p_dict.has(k)) {
			r_error = vformat("missing parameter '%s%s'", p_where, k);
			return ERR_INVALID_PARAMETER;
		}
	}
	return OK;
}

PackedStringArray expected_keys(const char *p_group) {
	PackedStringArray keys;
	if (p_group == nullptr) {
		keys.push_back("schema");
		keys.push_back("tier");
	}
	for (const IntField &f : kIntFields) {
		if ((f.group == nullptr && p_group == nullptr) || (f.group != nullptr && p_group != nullptr && strcmp(f.group, p_group) == 0)) {
			keys.push_back(f.key);
		}
	}
	if (p_group == nullptr) {
		for (const char *g : { "world", "placement", "plain", "town", "wall", "foliage", "beacon", "sh" }) {
			keys.push_back(g);
		}
	} else {
		for (const char *g : kClassGroup) {
			if (strcmp(g, p_group) == 0) {
				keys.push_back("style");
			}
		}
		if (strcmp(p_group, "beacon") == 0) {
			keys.push_back("cells");
		}
	}
	return keys;
}

} // namespace

Error params_from_dict(const Dictionary &p_dict, Params &r_params, String &r_error) {
	Params p;
	Error err = check_keys(p_dict, expected_keys(nullptr), "", r_error);
	if (err != OK) {
		return err;
	}
	if (p_dict["schema"].get_type() != Variant::STRING || String(p_dict["schema"]) != kParamsSchema) {
		r_error = vformat("parameter 'schema' must be \"%s\"", kParamsSchema);
		return ERR_INVALID_PARAMETER;
	}
	if (p_dict["tier"].get_type() != Variant::STRING) {
		r_error = "parameter 'tier' must be a string";
		return ERR_INVALID_PARAMETER;
	}
	p.tier = p_dict["tier"];
	for (const char *g : { "world", "placement", "plain", "town", "wall", "foliage", "beacon", "sh" }) {
		if (p_dict[g].get_type() != Variant::DICTIONARY) {
			r_error = vformat("parameter group '%s' must be a dictionary", g);
			return ERR_INVALID_PARAMETER;
		}
		err = check_keys(p_dict[g], expected_keys(g), String(g) + ".", r_error);
		if (err != OK) {
			return err;
		}
	}
	for (const IntField &f : kIntFields) {
		const Variant v = f.group == nullptr ? p_dict.get(f.key, Variant()) : Dictionary(p_dict[f.group]).get(f.key, Variant());
		int64_t iv = 0;
		if (!variant_to_int(v, iv)) {
			r_error = vformat("parameter '%s%s' must be an integer", f.group ? String(f.group) + "." : String(), f.key);
			return ERR_INVALID_PARAMETER;
		}
		p.*(f.member) = iv;
	}
	for (int c = 0; c < CLASS_COUNT; c++) {
		const Dictionary g = p_dict[kClassGroup[c]];
		if (g["style"].get_type() != Variant::DICTIONARY) {
			r_error = vformat("parameter '%s.style' must be a dictionary", kClassGroup[c]);
			return ERR_INVALID_PARAMETER;
		}
		const Dictionary style = g["style"];
		PackedStringArray skeys;
		for (const StyleField &s : kStyleFields) {
			skeys.push_back(s.key);
		}
		err = check_keys(style, skeys, String(kClassGroup[c]) + ".style.", r_error);
		if (err != OK) {
			return err;
		}
		for (const StyleField &s : kStyleFields) {
			int64_t iv = 0;
			if (!variant_to_int(style[s.key], iv)) {
				r_error = vformat("parameter '%s.style.%s' must be an integer", kClassGroup[c], s.key);
				return ERR_INVALID_PARAMETER;
			}
			p.style[c].*(s.member) = iv;
		}
	}
	const Variant cells_v = Dictionary(p_dict["beacon"]).get("cells", Variant());
	if (cells_v.get_type() != Variant::ARRAY) {
		r_error = "parameter 'beacon.cells' must be an array of integers";
		return ERR_INVALID_PARAMETER;
	}
	const Array cells = cells_v;
	for (int i = 0; i < cells.size(); i++) {
		int64_t iv = 0;
		if (!variant_to_int(cells[i], iv)) {
			r_error = "parameter 'beacon.cells' must be an array of integers";
			return ERR_INVALID_PARAMETER;
		}
		p.beacon_cells.push_back(iv);
	}
	r_params = p;
	return OK;
}

namespace {

Error merge_overrides(Dictionary &r_target, const Dictionary &p_over, const String &p_where, String &r_error) {
	for (const Variant &k : p_over.keys()) {
		const String key = k;
		if (!r_target.has(key)) {
			r_error = vformat("override of unknown parameter '%s%s'", p_where, key);
			return ERR_INVALID_PARAMETER;
		}
		const Variant v = p_over[k];
		if (v.get_type() == Variant::DICTIONARY && r_target[key].get_type() == Variant::DICTIONARY) {
			Dictionary sub = Dictionary(r_target[key]).duplicate(true);
			const Error err = merge_overrides(sub, v, p_where + key + ".", r_error);
			if (err != OK) {
				return err;
			}
			r_target[key] = sub;
		} else {
			r_target[key] = v;
		}
	}
	return OK;
}

} // namespace

Error apply_overrides(const Params &p_base, const Dictionary &p_overrides, Params &r_params, String &r_error) {
	Dictionary d = params_to_dict(p_base).duplicate(true);
	const Error err = merge_overrides(d, p_overrides, "", r_error);
	if (err != OK) {
		return err;
	}
	return params_from_dict(d, r_params, r_error);
}

String canonical_json(const Variant &p_value) {
	String out;
	json_emit(p_value, out);
	return out;
}

String params_sha(const Params &p_params) {
	return sha256_of_string(canonical_json(params_to_dict(p_params)));
}

String sha256_hex_of_canonical(const Variant &p_value) {
	return sha256_of_string(canonical_json(p_value));
}

Error validate_params(const Params &p, String &r_error) {
	auto fail = [&](const String &p_msg) {
		r_error = "invalid_params: " + p_msg;
		return ERR_INVALID_PARAMETER;
	};
	if (p.seed < 0) {
		return fail("seed must be >= 0");
	}
	if (p.tiles_x < 1 || p.tiles_z < 1 || p.tiles_x > 64 || p.tiles_z > 64) {
		return fail("tiles_x / tiles_z must be in [1, 64]");
	}
	if (!is_pow2(p.min_cell_m) || !is_pow2(p.base_cell_m) || p.base_cell_m < p.min_cell_m) {
		return fail("base_cell_m and min_cell_m must be powers of two with base >= min");
	}
	if (p.tile_size_m <= 0 || p.tile_size_m % p.base_cell_m != 0 || p.tile_size_m > 4096) {
		return fail("tile_size_m must be a positive multiple of base_cell_m, <= 4096");
	}
	if (p.max_chunk_splats < 1 || p.max_chunk_splats > 65536) {
		return fail("max_chunk_splats must be in [1, 65536] (GSQ page bound, ADR D-12)");
	}
	if (p.total_splats < 1 || p.total_splats > int64_t(UINT32_MAX)) {
		return fail("total_splats must be in [1, UINT32_MAX]");
	}
	if (p.margin_m < 0 || p.edge_margin_m < 0) {
		return fail("placement margins must be >= 0");
	}
	if (p.plain_y_jitter_q < 0 || p.plain_y_jitter_q > 65536 || p.ground_flatten_shift < 0 || p.ground_flatten_shift > 8) {
		return fail("plain.y_jitter_q must be in [0, 65536] and ground_flatten_shift in [0, 8]");
	}
	if (p.town_count < 0 || p.town_block_m <= 0 || p.town_size_m <= 0 || p.town_size_m % p.town_block_m != 0 ||
			p.min_cell_m % p.town_block_m != 0) {
		return fail("town.size_m must be a positive multiple of town.block_m, which must divide min_cell_m");
	}
	if (p.town_building_m <= 0 || p.town_building_m >= p.town_block_m || (p.town_block_m - p.town_building_m) % 2 != 0) {
		return fail("town.building_m must be in (0, block_m) with an even street width");
	}
	if (p.town_height_min_m < 1 || p.town_height_max_m < p.town_height_min_m || p.town_height_max_m > 256) {
		return fail("town heights must satisfy 1 <= min <= max <= 256");
	}
	if (p.town_density_per_m2 < 1 || p.town_roof_permille < 0 || p.town_roof_permille > 1000) {
		return fail("town.density_per_m2 >= 1 and roof_permille in [0, 1000]");
	}
	if (p.wall_length_m < 2 || p.wall_length_m % 2 != 0 || p.wall_height_m < 1 || p.wall_density_per_m2 < 1) {
		return fail("wall.length_m must be even and >= 2; height_m, density_per_m2 >= 1");
	}
	if (p.wall_footprint_m < 2 || p.wall_footprint_m % 2 != 0 || p.wall_separation_m % 2 != 0 ||
			p.wall_separation_m < p.wall_footprint_m) {
		return fail("wall.footprint_m must be even >= 2 and separation_m even >= footprint_m");
	}
	if (p.foliage_count < 0 || p.foliage_size_m < 1 || p.foliage_density_per_m2 < 1) {
		return fail("foliage.count >= 0, size_m >= 1, density_per_m2 >= 1");
	}
	if (p.foliage_canopy_min_q < 0 || p.foliage_canopy_max_q <= p.foliage_canopy_min_q || p.foliage_canopy_max_q > 1048576) {
		return fail("foliage canopy must satisfy 0 <= min < max <= 1048576 q");
	}
	if (!is_pow2(p.beacon_grid) || p.beacon_grid < 8 || p.beacon_grid > 256 || p.beacon_panel_m < 1 ||
			p.beacon_panel_m > p.base_cell_m || p.beacon_base_y_q < 0) {
		return fail("beacon.grid must be a power of two in [8, 256], panel_m in [1, base_cell_m]");
	}
	if (p.beacon_cells.size() % 2 != 0) {
		return fail("beacon.cells must hold (x, z) pairs");
	}
	const int64_t cells_per_tile = p.tile_size_m / p.base_cell_m;
	for (uint32_t i = 0; i < p.beacon_cells.size(); i++) {
		if (p.beacon_cells[i] < 0 || p.beacon_cells[i] >= cells_per_tile) {
			return fail("beacon.cells entries must be tile-local base-cell coordinates");
		}
	}
	if (p.sh_degree < 0 || p.sh_degree > 3) {
		return fail("sh.degree must be in [0, 3]");
	}
	if (p.sh_octaves < 1 || p.sh_corr_log2_m < 0 || p.sh_corr_log2_m > 10 || p.sh_corr_log2_m - 2 * (p.sh_octaves - 1) < 0) {
		return fail("sh.corr_log2_m in [0, 10] and every octave (corr / 4^o) at least 1 m");
	}
	if (p.sh_field_amp_q8 < 0 || p.sh_field_amp_q8 > 1024 || p.sh_jitter_amp < 0 || p.sh_jitter_amp > 32767) {
		return fail("sh.field_amp_q8 in [0, 1024], jitter_amp in [0, 32767]");
	}
	for (int64_t s : { p.sh_band_shift_1, p.sh_band_shift_2, p.sh_band_shift_3 }) {
		if (s < 0 || s > 8) {
			return fail("sh.band_shift_* must be in [0, 8]");
		}
	}
	for (int c = 0; c < CLASS_COUNT; c++) {
		const ClassStyle &s = p.style[c];
		if (s.sigma_min_q < 1 || s.sigma_max_q < s.sigma_min_q || s.sigma_max_q > 65536 ||
				(s.sigma_min_q >> p.ground_flatten_shift) < 1) {
			return fail(vformat("%s.style sigma must satisfy 1 <= min <= max <= 65536 and survive the flatten shift", kClassGroup[c]));
		}
		if (s.opacity_min_q8 < 0 || s.opacity_max_q8 < s.opacity_min_q8 || s.opacity_max_q8 > 255) {
			return fail(vformat("%s.style opacity must satisfy 0 <= min <= max <= 255", kClassGroup[c]));
		}
		for (int64_t v : { s.color_r, s.color_g, s.color_b }) {
			if (v < 0 || v > 255) {
				return fail(vformat("%s.style colour channels must be in [0, 255]", kClassGroup[c]));
			}
		}
		if (c == CLASS_BEACON && s.color_var_q8 != 0) {
			return fail("beacon.style.color_var_q8 must be 0: beacons are flat-coded panels");
		}
		if (s.color_var_q8 < 0 || s.color_var_q8 > 255) {
			return fail(vformat("%s.style.color_var_q8 must be in [0, 255]", kClassGroup[c]));
		}
	}
	return OK;
}

// ---------------------------------------------------------------------------
// Plan.
// ---------------------------------------------------------------------------

namespace {

int64_t feature_count_in(const Params &p, const Feature &f, const IRect &cell) {
	switch (f.cls) {
		case CLASS_TOWN:
			return p.town_density_per_m2 * f.footprint.intersection(cell).area();
		case CLASS_FOLIAGE:
			return p.foliage_density_per_m2 * f.footprint.intersection(cell).area();
		case CLASS_WALL: {
			if (f.plane_z_m < cell.z0 || f.plane_z_m >= cell.z1) {
				return 0;
			}
			const int64_t len = MIN(f.footprint.x1, cell.x1) - MAX(f.footprint.x0, cell.x0);
			return len > 0 ? p.wall_density_per_m2 * p.wall_height_m * len : 0;
		}
		case CLASS_BEACON: {
			const bool inside = f.beacon_center_x_m >= cell.x0 && f.beacon_center_x_m < cell.x1 &&
					f.beacon_center_z_m >= cell.z0 && f.beacon_center_z_m < cell.z1;
			return inside ? p.beacon_grid * p.beacon_grid : 0;
		}
		default:
			return 0;
	}
}

bool feature_excludes_plain(const Feature &f) {
	return f.cls == CLASS_TOWN || f.cls == CLASS_FOLIAGE || f.cls == CLASS_WALL;
}

struct PlanBuilder {
	const Params &p;
	Plan &plan;
	String &err;
	uint64_t plain_total = 0;

	PlanBuilder(const Params &p_p, Plan &r_plan, String &r_err) :
			p(p_p), plan(r_plan), err(r_err) {}

	uint64_t cell_need(const IRect &cell) const {
		uint64_t fixed = 0;
		int64_t excl = 0;
		for (const Feature &f : plan.features) {
			if (!f.footprint.intersects(cell) && f.cls != CLASS_BEACON && f.cls != CLASS_WALL) {
				continue;
			}
			fixed += uint64_t(feature_count_in(p, f, cell));
			if (feature_excludes_plain(f)) {
				excl += f.footprint.intersection(cell).area();
			}
		}
		const uint64_t a = uint64_t(cell.area() - excl);
		const uint64_t A = uint64_t(plan.plain_area);
		const uint64_t plain_ceil = A == 0 ? 0 : (plain_total * a + A - 1u) / A;
		return fixed + plain_ceil;
	}

	Error subdivide(const IRect &cell, LocalVector<IRect> &r_leaves) {
		if (cell_need(cell) <= uint64_t(p.max_chunk_splats)) {
			r_leaves.push_back(cell);
			return OK;
		}
		const int64_t half = cell.width() / 2;
		if (half < p.min_cell_m) {
			err = vformat("plan: a %d m cell at (%d, %d) needs %s splats > max_chunk_splats %d at the minimum cell size",
					cell.width(), cell.x0, cell.z0, String::num_uint64(cell_need(cell)), p.max_chunk_splats);
			return ERR_INVALID_PARAMETER;
		}
		for (int q = 0; q < 4; q++) {
			const int64_t qx = q & 1;
			const int64_t qz = q >> 1;
			const IRect child{ cell.x0 + qx * half, cell.z0 + qz * half, cell.x0 + (qx + 1) * half, cell.z0 + (qz + 1) * half };
			const Error e = subdivide(child, r_leaves);
			if (e != OK) {
				return e;
			}
		}
		return OK;
	}

	bool blocked(const IRect &cand, const LocalVector<IRect> &reserved) const {
		const IRect g = cand.grown(p.margin_m);
		for (const IRect &r : reserved) {
			if (g.intersects(r)) {
				return true;
			}
		}
		return false;
	}

	Error place(ContentClass cls, int64_t count, int64_t size, int64_t align, int64_t density, LocalVector<IRect> &reserved) {
		const IRect &w = plan.world;
		const int64_t span_x = w.width() - 2 * p.edge_margin_m - size;
		const int64_t span_z = w.depth() - 2 * p.edge_margin_m - size;
		if (span_x < 0 || span_z < 0) {
			err = vformat("plan: a %d m %s does not fit inside the world edge margin", size, class_name(cls));
			return ERR_INVALID_PARAMETER;
		}
		for (int64_t i = 0; i < count; i++) {
			bool placed = false;
			for (int attempt = 0; attempt < kMaxPlacementAttempts && !placed; attempt++) {
				const uint64_t h = hash5(uint64_t(p.seed), DOM_PLACE, uint64_t(cls), uint64_t(i), uint64_t(attempt));
				const int64_t ox = align * int64_t((h & 0xFFFFFFFFull) % uint64_t(span_x / align + 1));
				const int64_t oz = align * int64_t((h >> 32u) % uint64_t(span_z / align + 1));
				const IRect cand{ w.x0 + p.edge_margin_m + ox, w.z0 + p.edge_margin_m + oz,
					w.x0 + p.edge_margin_m + ox + size, w.z0 + p.edge_margin_m + oz + size };
				if (blocked(cand, reserved)) {
					continue;
				}
				Feature f;
				f.cls = cls;
				f.index = uint32_t(i);
				f.footprint = cand;
				f.count = density * cand.area();
				plan.features.push_back(f);
				reserved.push_back(cand);
				placed = true;
			}
			if (!placed) {
				err = vformat("plan: could not place %s %d without overlap after %d attempts", class_name(cls), i, kMaxPlacementAttempts);
				return ERR_INVALID_PARAMETER;
			}
		}
		return OK;
	}

	Error build() {
		plan.params = p;
		plan.params_sha = params_sha(p);
		const int64_t W = p.tile_size_m * p.tiles_x;
		const int64_t D = p.tile_size_m * p.tiles_z;
		// Centred on the origin; tile and cell edges stay on the origin-aligned lattice.
		const int64_t x0 = -(W / 2);
		const int64_t z0 = -(D / 2);
		if (x0 % p.base_cell_m != 0 || z0 % p.base_cell_m != 0) {
			err = "plan: the world origin offset must lie on the base-cell lattice";
			return ERR_INVALID_PARAMETER;
		}
		plan.world = IRect{ x0, z0, x0 + W, z0 + D };

		LocalVector<IRect> reserved;
		// Beacons: per tile, at the tile-local base-cell centres; ids row-major by tile.
		const uint32_t beacons_per_tile = p.beacon_cells.size() / 2u;
		uint32_t beacon_id = 0;
		for (int64_t tz = 0; tz < p.tiles_z; tz++) {
			for (int64_t tx = 0; tx < p.tiles_x; tx++) {
				for (uint32_t b = 0; b < beacons_per_tile; b++) {
					const int64_t cx = p.beacon_cells[2 * b];
					const int64_t cz = p.beacon_cells[2 * b + 1];
					const IRect cell{ x0 + tx * p.tile_size_m + cx * p.base_cell_m, z0 + tz * p.tile_size_m + cz * p.base_cell_m,
						x0 + tx * p.tile_size_m + (cx + 1) * p.base_cell_m, z0 + tz * p.tile_size_m + (cz + 1) * p.base_cell_m };
					Feature f;
					f.cls = CLASS_BEACON;
					f.index = beacon_id;
					f.beacon_id = beacon_id;
					f.beacon_code = beacon_code(uint16_t(beacon_id));
					f.beacon_center_x_m = cell.x0 + p.base_cell_m / 2;
					f.beacon_center_z_m = cell.z0 + p.base_cell_m / 2;
					f.footprint = IRect{ f.beacon_center_x_m - p.beacon_panel_m / 2, f.beacon_center_z_m,
						f.beacon_center_x_m - p.beacon_panel_m / 2 + p.beacon_panel_m, f.beacon_center_z_m + 1 };
					f.count = p.beacon_grid * p.beacon_grid;
					plan.features.push_back(f);
					reserved.push_back(cell);
					beacon_id++;
				}
			}
		}
		if (beacon_id > 65536u) {
			err = "plan: more than 65,536 beacons cannot carry a 16-bit id";
			return ERR_INVALID_PARAMETER;
		}
		// The corridor: two walls along X, centred on x = 0.
		{
			const int64_t half_sep = p.wall_separation_m / 2;
			const int64_t half_fp = p.wall_footprint_m / 2;
			for (int i = 0; i < 2; i++) {
				Feature f;
				f.cls = CLASS_WALL;
				f.index = uint32_t(i);
				f.plane_z_m = p.wall_corridor_z_m + (i == 0 ? -half_sep : half_sep);
				f.footprint = IRect{ -(p.wall_length_m / 2), f.plane_z_m - half_fp, p.wall_length_m / 2, f.plane_z_m + half_fp };
				f.count = p.wall_density_per_m2 * p.wall_height_m * p.wall_length_m;
				if (f.footprint.x0 < x0 + p.edge_margin_m || f.footprint.x1 > x0 + W - p.edge_margin_m ||
						f.footprint.z0 < z0 + p.edge_margin_m || f.footprint.z1 > z0 + D - p.edge_margin_m) {
					err = "plan: the corridor walls leave the world edge margin";
					return ERR_INVALID_PARAMETER;
				}
				if (f.plane_z_m % p.min_cell_m == 0) {
					err = "plan: a wall plane must not lie on a cell edge";
					return ERR_INVALID_PARAMETER;
				}
				for (const IRect &r : reserved) {
					if (f.footprint.grown(p.margin_m).intersects(r)) {
						err = "plan: the corridor walls cross a beacon cell";
						return ERR_INVALID_PARAMETER;
					}
				}
				plan.features.push_back(f);
			}
			reserved.push_back(plan.features[plan.features.size() - 2].footprint);
			reserved.push_back(plan.features[plan.features.size() - 1].footprint);
		}
		Error e = place(CLASS_TOWN, p.town_count, p.town_size_m, p.town_block_m, p.town_density_per_m2, reserved);
		if (e != OK) {
			return e;
		}
		e = place(CLASS_FOLIAGE, p.foliage_count, p.foliage_size_m, 1, p.foliage_density_per_m2, reserved);
		if (e != OK) {
			return e;
		}

		// Exact class totals; the plain is the remainder.
		uint64_t fixed = 0;
		int64_t excl = 0;
		for (const Feature &f : plan.features) {
			plan.class_totals[f.cls] += uint64_t(f.count);
			plan.class_instances[f.cls]++;
			fixed += uint64_t(f.count);
			if (feature_excludes_plain(f)) {
				excl += f.footprint.area();
			}
		}
		if (fixed >= uint64_t(p.total_splats)) {
			err = vformat("plan: the features need %s splats, at least total_splats %d", String::num_uint64(fixed), p.total_splats);
			return ERR_INVALID_PARAMETER;
		}
		plain_total = uint64_t(p.total_splats) - fixed;
		plan.plain_area = plan.world.area() - excl;
		plan.class_totals[CLASS_PLAIN] = plain_total;
		plan.class_instances[CLASS_PLAIN] = 1;

		// Cells: base lattice, power-of-two subdivision, Morton order.
		LocalVector<IRect> leaves;
		for (int64_t z = z0; z < z0 + D; z += p.base_cell_m) {
			for (int64_t x = x0; x < x0 + W; x += p.base_cell_m) {
				e = subdivide(IRect{ x, z, x + p.base_cell_m, z + p.base_cell_m }, leaves);
				if (e != OK) {
					return e;
				}
			}
		}
		LocalVector<uint64_t> keys;
		keys.resize(leaves.size());
		for (uint32_t i = 0; i < leaves.size(); i++) {
			keys[i] = morton2(uint32_t((leaves[i].x0 - x0) / p.min_cell_m), uint32_t((leaves[i].z0 - z0) / p.min_cell_m));
		}
		LocalVector<uint32_t> order;
		order.resize(leaves.size());
		for (uint32_t i = 0; i < order.size(); i++) {
			order[i] = i;
		}
		std::sort(order.ptr(), order.ptr() + order.size(), [&keys](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });

		// Plain areas and the largest-remainder apportionment of the plain total.
		const uint64_t A = uint64_t(plan.plain_area);
		plan.chunks.resize(leaves.size());
		LocalVector<uint64_t> remainders;
		remainders.resize(leaves.size());
		uint64_t assigned = 0;
		for (uint32_t c = 0; c < order.size(); c++) {
			ChunkPlan &cp = plan.chunks[c];
			cp.cell = leaves[order[c]];
			cp.morton = keys[order[c]];
			int64_t cexcl = 0;
			for (const Feature &f : plan.features) {
				if (feature_excludes_plain(f) && f.footprint.intersects(cp.cell)) {
					cexcl += f.footprint.intersection(cp.cell).area();
				}
			}
			cp.plain_area = cp.cell.area() - cexcl;
			const uint64_t num = plain_total * uint64_t(cp.plain_area);
			const uint64_t q = A == 0 ? 0 : num / A;
			remainders[c] = A == 0 ? 0 : num % A;
			cp.class_counts[CLASS_PLAIN] = uint32_t(q);
			assigned += q;
		}
		uint64_t deficit = plain_total - assigned;
		if (deficit > 0) {
			LocalVector<uint32_t> by_rem;
			by_rem.resize(plan.chunks.size());
			for (uint32_t i = 0; i < by_rem.size(); i++) {
				by_rem[i] = i;
			}
			std::sort(by_rem.ptr(), by_rem.ptr() + by_rem.size(), [&remainders](uint32_t a, uint32_t b) {
				return remainders[a] != remainders[b] ? remainders[a] > remainders[b] : a < b;
			});
			for (uint32_t i = 0; i < by_rem.size() && deficit > 0; i++) {
				if (plan.chunks[by_rem[i]].plain_area > 0) {
					plan.chunks[by_rem[i]].class_counts[CLASS_PLAIN]++;
					deficit--;
				}
			}
		}
		if (deficit != 0) {
			err = "plan: the plain apportionment did not place every plain splat";
			return ERR_BUG;
		}

		// Parts (plain first, then features in feature order) and the plain rectangles.
		uint64_t grand = 0;
		for (uint32_t c = 0; c < plan.chunks.size(); c++) {
			ChunkPlan &cp = plan.chunks[c];
			cp.first_part = plan.parts.size();
			cp.first_plain_rect = plan.plain_rects.size();
			if (cp.class_counts[CLASS_PLAIN] > 0) {
				ChunkPart part;
				part.cls = CLASS_PLAIN;
				part.rect = cp.cell;
				part.count = cp.class_counts[CLASS_PLAIN];
				plan.parts.push_back(part);
			}
			LocalVector<IRect> rects;
			rects.push_back(cp.cell);
			for (uint32_t fi = 0; fi < plan.features.size(); fi++) {
				const Feature &f = plan.features[fi];
				const int64_t n = feature_count_in(p, f, cp.cell);
				if (n > 0) {
					ChunkPart part;
					part.cls = f.cls;
					part.feature = fi;
					part.count = uint32_t(n);
					if (f.cls == CLASS_WALL) {
						part.rect = IRect{ MAX(f.footprint.x0, cp.cell.x0), f.plane_z_m, MIN(f.footprint.x1, cp.cell.x1), f.plane_z_m };
					} else if (f.cls == CLASS_BEACON) {
						if (f.footprint.x0 < cp.cell.x0 || f.footprint.x1 > cp.cell.x1) {
							err = vformat("plan: beacon %d's panel straddles a chunk edge", f.beacon_id);
							return ERR_BUG;
						}
						part.rect = f.footprint;
					} else {
						part.rect = f.footprint.intersection(cp.cell);
					}
					plan.parts.push_back(part);
					cp.class_counts[f.cls] += uint32_t(n);
				}
				if (feature_excludes_plain(f) && f.footprint.intersects(cp.cell)) {
					LocalVector<IRect> next;
					for (const IRect &r : rects) {
						if (!r.intersects(f.footprint)) {
							next.push_back(r);
							continue;
						}
						const IRect in = r.intersection(f.footprint);
						if (in.z0 > r.z0) {
							next.push_back(IRect{ r.x0, r.z0, r.x1, in.z0 });
						}
						if (in.z1 < r.z1) {
							next.push_back(IRect{ r.x0, in.z1, r.x1, r.z1 });
						}
						if (in.x0 > r.x0) {
							next.push_back(IRect{ r.x0, in.z0, in.x0, in.z1 });
						}
						if (in.x1 < r.x1) {
							next.push_back(IRect{ in.x1, in.z0, r.x1, in.z1 });
						}
					}
					rects = next;
				}
			}
			int64_t rect_area = 0;
			for (const IRect &r : rects) {
				if (r.area() > 0) {
					plan.plain_rects.push_back(r);
					rect_area += r.area();
				}
			}
			if (rect_area != cp.plain_area) {
				err = "plan: plain rectangles do not tile the chunk's plain area";
				return ERR_BUG;
			}
			cp.part_count = plan.parts.size() - cp.first_part;
			if (cp.part_count > 256u) {
				err = vformat("plan: chunk %d has %d content parts (bound 256)", c, cp.part_count);
				return ERR_INVALID_PARAMETER;
			}
			cp.plain_rect_count = plan.plain_rects.size() - cp.first_plain_rect;
			uint64_t n = 0;
			for (int k = 0; k < CLASS_COUNT; k++) {
				n += cp.class_counts[k];
			}
			if (n > uint64_t(p.max_chunk_splats) || n == 0) {
				err = vformat("plan: chunk %d holds %s splats (bound %d)", c, String::num_uint64(n), p.max_chunk_splats);
				return n == 0 ? ERR_INVALID_PARAMETER : ERR_BUG;
			}
			cp.count = uint32_t(n);
			plan.max_chunk_count = MAX(plan.max_chunk_count, cp.count);
			grand += n;
		}
		uint64_t class_sum = 0;
		for (int k = 0; k < CLASS_COUNT; k++) {
			uint64_t s = 0;
			for (const ChunkPlan &cp : plan.chunks) {
				s += cp.class_counts[k];
			}
			if (s != plan.class_totals[k]) {
				err = vformat("plan: class %s chunks sum to %s, expected %s", class_name(ContentClass(k)), String::num_uint64(s),
						String::num_uint64(plan.class_totals[k]));
				return ERR_BUG;
			}
			class_sum += s;
		}
		// §6.2: "exact; asserted by the generator".
		if (grand != uint64_t(p.total_splats) || class_sum != grand) {
			err = vformat("plan: chunks hold %s splats, the declared total is %d", String::num_uint64(grand), p.total_splats);
			return ERR_BUG;
		}
		plan.total_splats = grand;

		// SH layout and the seeded mixing weights.
		const uint32_t coeffs = uint32_t((p.sh_degree + 1) * (p.sh_degree + 1) - 1);
		plan.sh_first_order = MIN(coeffs, 3u);
		plan.sh_high_order = coeffs > 3u ? coeffs - 3u : 0u;
		plan.sh_weights.resize(coeffs * 3u * kLatents);
		for (uint32_t c = 0; c < coeffs * 3u; c++) {
			for (int k = 0; k < kLatents; k++) {
				plan.sh_weights[c * kLatents + k] = int32_t(hash4(uint64_t(p.seed), DOM_SH_WEIGHT, c, uint64_t(k)) & 0xFFu) - 128;
			}
		}

		// Conservative analytic world bounds (the header is declared before any splat
		// exists); generate_world checks every chunk AABB against them.
		int64_t sigma_max = 0;
		for (int k = 0; k < CLASS_COUNT; k++) {
			sigma_max = MAX(sigma_max, p.style[k].sigma_max_q);
		}
		const int64_t pad_q = 3 * sigma_max + 4;
		int64_t ymax_q = p.plain_y_jitter_q;
		ymax_q = MAX(ymax_q, p.town_height_max_m * 4096);
		ymax_q = MAX(ymax_q, p.wall_height_m * 4096);
		ymax_q = MAX(ymax_q, p.foliage_canopy_max_q);
		ymax_q = MAX(ymax_q, p.beacon_base_y_q + p.beacon_panel_m * 4096);
		const float pad = q12_to_float(pad_q);
		plan.world_bounds = AABB(Vector3(float(x0) - pad, -pad, float(z0) - pad),
				Vector3(float(W) + 2.0f * pad, q12_to_float(ymax_q) + 2.0f * pad, float(D) + 2.0f * pad));
		return OK;
	}
};

} // namespace

Error build_plan(const Params &p_params, Plan &r_plan, String &r_error) {
	r_plan = Plan();
	Error err = validate_params(p_params, r_error);
	if (err != OK) {
		return err;
	}
	PlanBuilder b(p_params, r_plan, r_error);
	err = b.build();
	if (err != OK && !r_error.begins_with("plan:")) {
		r_error = "plan: " + r_error;
	}
	return err;
}

uint64_t expected_file_bytes(const Plan &p_plan, const LocalVector<uint32_t> &p_selection) {
	uint64_t splats = 0;
	uint64_t chunks = 0;
	if (p_selection.is_empty()) {
		splats = p_plan.total_splats;
		chunks = p_plan.chunks.size();
	} else {
		for (uint32_t c : p_selection) {
			if (c < p_plan.chunks.size()) {
				splats += p_plan.chunks[c].count;
				chunks++;
			}
		}
	}
	// The metadata section is small and not counted.
	return gs_world_format::kPrefixBytes + splats * sizeof(Gaussian) + splats * p_plan.sh_high_order * sizeof(Vector3) +
			chunks * gs_world_format::kChunkRecordBytes + splats * sizeof(uint32_t);
}

// ---------------------------------------------------------------------------
// Per-splat generation.
// ---------------------------------------------------------------------------

namespace {

struct Latent {
	int64_t q[kLatents];
};

inline int64_t smooth6(int64_t t) {
	// 3*64*t^2 - 2*t^3 on t in [0, 64]: 0 .. 2^18.
	return 192 * t * t - 2 * t * t * t;
}

Latent latent_at(const Plan &plan, int64_t X, int64_t Z) {
	const Params &p = plan.params;
	Latent out;
	for (int k = 0; k < kLatents; k++) {
		int64_t acc = 0;
		for (int64_t o = 0; o < p.sh_octaves; o++) {
			const int shift = int(kFpBits + (p.sh_corr_log2_m - 2 * o) - 6);
			const int64_t xn = (X + kNoiseOffset) >> shift;
			const int64_t zn = (Z + kNoiseOffset) >> shift;
			const int64_t ix = xn >> 6;
			const int64_t iz = zn >> 6;
			const int64_t sx = smooth6(xn & 63);
			const int64_t sz = smooth6(zn & 63);
			const uint64_t dom = DOM_NOISE + uint64_t(k) * 16u + uint64_t(o);
			auto v = [&](int64_t a, int64_t b) {
				return int64_t(hash4(uint64_t(p.seed), dom, uint64_t(a), uint64_t(b)) & 0xFFu) - 128;
			};
			const int64_t one = int64_t(1) << 18;
			const int64_t bil = v(ix, iz) * (one - sx) * (one - sz) + v(ix + 1, iz) * sx * (one - sz) +
					v(ix, iz + 1) * (one - sx) * sz + v(ix + 1, iz + 1) * sx * sz;
			acc += bil / (int64_t(1) << o);
		}
		out.q[k] = acc / (int64_t(1) << 28);
	}
	return out;
}

struct RngStream {
	uint64_t base;
	uint32_t i = 0;
	uint64_t next() { return mix64(base + kGolden * uint64_t(++i)); }
};

void unit_quat_yaw(uint64_t r, Quaternion &q) {
	int64_t a = int64_t(r & 0xFFu) - 128;
	int64_t b = int64_t((r >> 8u) & 0xFFu) - 128;
	if (a == 0 && b == 0) {
		a = 127;
	}
	const uint64_t s = isqrt_floor(uint64_t(a * a + b * b) << 32u); // |v| * 2^16
	q.x = 0.0f;
	q.y = float(double((b * (int64_t(1) << 32)) / int64_t(s)) * (1.0 / 65536.0));
	q.z = 0.0f;
	q.w = float(double((a * (int64_t(1) << 32)) / int64_t(s)) * (1.0 / 65536.0));
}

void unit_quat_full(uint64_t r, Quaternion &q) {
	int64_t c[4];
	for (int i = 0; i < 4; i++) {
		c[i] = int64_t((r >> (8u * i)) & 0xFFu) - 128;
	}
	if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) {
		c[3] = 127;
	}
	const uint64_t s = isqrt_floor(uint64_t(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]) << 32u);
	float out[4];
	for (int i = 0; i < 4; i++) {
		out[i] = float(double((c[i] * (int64_t(1) << 32)) / int64_t(s)) * (1.0 / 65536.0));
	}
	q.x = out[0];
	q.y = out[1];
	q.z = out[2];
	q.w = out[3];
}

inline int64_t clamp_i(int64_t v, int64_t lo, int64_t hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

struct ChunkJob {
	const Plan *plan = nullptr;
	const ChunkPlan *cp = nullptr;
	const uint32_t *order = nullptr; // (part << 16) | k
	Gaussian *g = nullptr;
	Vector3 *sh = nullptr;
	uint32_t count = 0;
};

void make_splat(const Plan &plan, const ChunkPlan &cp, uint32_t part_index, uint32_t k, Gaussian &g, Vector3 *sh_high) {
	const Params &p = plan.params;
	const ChunkPart &part = plan.parts[cp.first_part + part_index];
	const ClassStyle &st = p.style[part.cls];
	const uint64_t part_seed = hash5(uint64_t(p.seed), DOM_PART, uint64_t(cp.cell.x0), uint64_t(cp.cell.z0),
			(uint64_t(part.cls) << 32u) | uint64_t(part.feature));
	const uint32_t U = uint32_t(part_seed) + k * kR2A1;
	const uint32_t V = uint32_t(part_seed >> 32u) + k * kR2A2;
	const int64_t u24 = int64_t(U >> 8u);
	const int64_t v24 = int64_t(V >> 8u);
	RngStream rng{ hash4(part_seed, DOM_SPLAT, k, 0) };
	const uint64_t r0 = rng.next();
	const uint64_t r1 = rng.next();
	const uint64_t r2 = rng.next();

	int64_t X = 0, Y = 0, Z = 0;
	int flatten_axis = 1; // 0 x, 1 y, 2 z, -1 none
	enum { ROT_YAW, ROT_IDENTITY, ROT_FULL } rot = ROT_YAW;
	Vector3 normal(0, 1, 0);
	int64_t color_override = -1;
	bool zero_sh = false;

	switch (part.cls) {
		case CLASS_PLAIN: {
			const IRect *rects = plan.plain_rects.ptr() + cp.first_plain_rect;
			const uint64_t A = uint64_t(cp.plain_area);
			const uint64_t T = uint64_t(U) * A;
			uint64_t cum = 0;
			uint32_t r = 0;
			for (; r + 1u < cp.plain_rect_count; r++) {
				const uint64_t a = uint64_t(rects[r].area());
				if (T < (cum + a) << 32u) {
					break;
				}
				cum += a;
			}
			const IRect &rc = rects[r];
			const uint64_t N = T - (cum << 32u);
			const int64_t uu = int64_t(N / (uint64_t(rc.area()) << 8u));
			X = rc.x0 * kFpOne + rc.width() * MIN(uu, int64_t(kFpOne - 1));
			Z = rc.z0 * kFpOne + rc.depth() * v24;
			Y = int64_t(r0 % uint64_t(p.plain_y_jitter_q + 1)) << kQToFpShift;
		} break;
		case CLASS_TOWN: {
			const Feature &f = plan.features[part.feature];
			X = part.rect.x0 * kFpOne + part.rect.width() * u24;
			Z = part.rect.z0 * kFpOne + part.rect.depth() * v24;
			const int64_t block = p.town_block_m * kFpOne;
			const int64_t lx = X - f.footprint.x0 * kFpOne;
			const int64_t lz = Z - f.footprint.z0 * kFpOne;
			const int64_t bx = lx / block;
			const int64_t bz = lz / block;
			const int64_t ox = lx - bx * block;
			const int64_t oz = lz - bz * block;
			const int64_t inset = ((p.town_block_m - p.town_building_m) / 2) * kFpOne;
			const int64_t bmax = inset + p.town_building_m * kFpOne;
			if (ox >= inset && ox < bmax && oz >= inset && oz < bmax) {
				const int64_t H = p.town_height_min_m +
						int64_t(hash5(uint64_t(p.seed), DOM_BUILDING, f.index, uint64_t(bx), uint64_t(bz)) %
								uint64_t(p.town_height_max_m - p.town_height_min_m + 1));
				if (int64_t(r1 % 1000u) < p.town_roof_permille) {
					Y = H * kFpOne;
				} else {
					const uint32_t face = uint32_t(r1 >> 40u) & 3u;
					const int64_t bx0 = f.footprint.x0 * kFpOne + bx * block;
					const int64_t bz0 = f.footprint.z0 * kFpOne + bz * block;
					rot = ROT_IDENTITY;
					if (face < 2u) {
						X = bx0 + (face == 0u ? inset : bmax);
						flatten_axis = 0;
						normal = Vector3(face == 0u ? -1.0f : 1.0f, 0, 0);
					} else {
						Z = bz0 + (face == 2u ? inset : bmax);
						flatten_axis = 2;
						normal = Vector3(0, 0, face == 2u ? -1.0f : 1.0f);
					}
					Y = H * int64_t(r2 & 0xFFFFFFu);
				}
			} else {
				Y = int64_t(r0 % uint64_t(p.plain_y_jitter_q + 1)) << kQToFpShift;
			}
		} break;
		case CLASS_WALL: {
			const Feature &f = plan.features[part.feature];
			X = part.rect.x0 * kFpOne + part.rect.width() * u24;
			Y = p.wall_height_m * v24;
			Z = f.plane_z_m * kFpOne;
			flatten_axis = 2;
			rot = ROT_IDENTITY;
			normal = Vector3(0, 0, f.index == 0u ? 1.0f : -1.0f);
		} break;
		case CLASS_FOLIAGE: {
			X = part.rect.x0 * kFpOne + part.rect.width() * u24;
			Z = part.rect.z0 * kFpOne + part.rect.depth() * v24;
			Y = (p.foliage_canopy_min_q << kQToFpShift) +
					(((p.foliage_canopy_max_q - p.foliage_canopy_min_q) * int64_t(r1 & 0xFFFFFFu)) >> 12);
			flatten_axis = -1;
			rot = ROT_FULL;
		} break;
		case CLASS_BEACON: {
			const Feature &f = plan.features[part.feature];
			const uint32_t bits = log2_exact(p.beacon_grid * p.beacon_grid);
			const uint32_t idx = bit_reverse(k, bits);
			const int64_t grid = p.beacon_grid;
			const int64_t gx = int64_t(idx) % grid;
			const int64_t gy = int64_t(idx) / grid;
			const int64_t step2 = (p.beacon_panel_m * kFpOne) / (2 * grid); // half a grid step, exact
			X = f.footprint.x0 * kFpOne + (2 * gx + 1) * step2;
			Y = (p.beacon_base_y_q << kQToFpShift) + (2 * gy + 1) * step2;
			Z = f.beacon_center_z_m * kFpOne;
			const int64_t col = gx * 8 / grid;
			const int64_t row = gy * 8 / grid;
			const bool bit = ((f.beacon_code >> uint64_t(row * 8 + col)) & 1u) != 0u;
			color_override = bit ? 1 : 0;
			flatten_axis = 2;
			rot = ROT_IDENTITY;
			normal = Vector3(0, 0, 1);
			zero_sh = true;
		} break;
		default:
			break;
	}

	g = Gaussian();
	g.position = Vector3(fp_to_float(X), fp_to_float(Y), fp_to_float(Z));

	const uint64_t r3 = rng.next();
	const int64_t sigma_q = st.sigma_min_q + ((st.sigma_max_q - st.sigma_min_q) * int64_t(r3 & 0xFFu)) / 255;
	const float s = q12_to_float(sigma_q);
	const float sf = q12_to_float(sigma_q >> p.ground_flatten_shift);
	g.scale = Vector3(flatten_axis == 0 ? sf : s, flatten_axis == 1 ? sf : s, flatten_axis == 2 ? sf : s);

	const int64_t o8 = st.opacity_min_q8 + ((st.opacity_max_q8 - st.opacity_min_q8) * int64_t((r3 >> 8u) & 0xFFu)) / 255;
	g.opacity = float(double(o8) * (1.0 / 256.0));

	switch (rot) {
		case ROT_YAW:
			unit_quat_yaw(r3 >> 16u, g.rotation);
			break;
		case ROT_FULL:
			unit_quat_full(r3 >> 16u, g.rotation);
			break;
		case ROT_IDENTITY:
			g.rotation = Quaternion();
			break;
	}

	const Latent lat = latent_at(plan, X, Z);
	const uint64_t r4 = rng.next();
	int64_t rgb[3];
	if (color_override >= 0) {
		// Beacon code bit 1 = the style colour, 0 = its complement.
		const int64_t c[3] = { st.color_r, st.color_g, st.color_b };
		for (int i = 0; i < 3; i++) {
			rgb[i] = color_override == 1 ? c[i] : 255 - c[i];
		}
	} else {
		const int64_t var = st.color_var_q8;
		const int64_t base = lat.q[0] * var;
		const int64_t d[3] = { base, base + lat.q[1] * var / 2, base + lat.q[2] * var / 2 };
		const int64_t c[3] = { st.color_r, st.color_g, st.color_b };
		for (int i = 0; i < 3; i++) {
			const int64_t jitter = int64_t((r4 >> (8u * i)) & 7u) - 3;
			rgb[i] = clamp_i(c[i] + d[i] / 65536 + jitter, 0, 255);
		}
	}
	g.sh_dc = Color(float(double(rgb[0] - 128) * (1.0 / 256.0)), float(double(rgb[1] - 128) * (1.0 / 256.0)),
			float(double(rgb[2] - 128) * (1.0 / 256.0)), 1.0f);
	g.normal = normal;
	g.area = 1.0f;
	g.brush_axes = Vector2(1.0f, 1.0f);
	g.painterly_meta = gaussian_pack_painterly_meta(0);
	g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);

	const uint32_t coeffs = plan.sh_first_order + plan.sh_high_order;
	uint64_t jitter_bits = 0;
	uint32_t jitter_left = 0;
	for (uint32_t m = 0; m < coeffs; m++) {
		const int64_t shift = m < 3u ? p.sh_band_shift_1 : (m < 8u ? p.sh_band_shift_2 : p.sh_band_shift_3);
		const double scale = 1.0 / double(int64_t(1) << (14 + shift));
		float ch[3];
		for (int c = 0; c < 3; c++) {
			if (jitter_left == 0u) {
				jitter_bits = rng.next();
				jitter_left = 8u;
			}
			const int64_t jb = int64_t(jitter_bits & 0xFFu) - 128;
			jitter_bits >>= 8u;
			jitter_left--;
			if (zero_sh) {
				ch[c] = 0.0f;
				continue;
			}
			const int32_t *w = plan.sh_weights.ptr() + (m * 3u + uint32_t(c)) * kLatents;
			int64_t acc = 0;
			for (int k2 = 0; k2 < kLatents; k2++) {
				acc += int64_t(w[k2]) * lat.q[k2];
			}
			const int64_t field = (acc * p.sh_field_amp_q8) / 65536;
			const int64_t jitter = (jb * p.sh_jitter_amp) / 128;
			const int64_t total = clamp_i(field + jitter, -32767, 32767);
			ch[c] = float(double(total) * scale);
		}
		const Vector3 v(ch[0], ch[1], ch[2]);
		if (m < 3u) {
			g.sh_1[m] = v;
		} else {
			sh_high[m - 3u] = v;
		}
	}
}

void chunk_task(void *p_userdata, uint32_t p_block) {
	ChunkJob &job = *static_cast<ChunkJob *>(p_userdata);
	const uint32_t begin = p_block * kSplatsPerTask;
	const uint32_t end = MIN(job.count, begin + kSplatsPerTask);
	const uint32_t high = job.plan->sh_high_order;
	for (uint32_t i = begin; i < end; i++) {
		const uint32_t e = job.order[i];
		make_splat(*job.plan, *job.cp, e >> 16u, e & 0xFFFFu, job.g[i], high ? job.sh + uint64_t(i) * high : nullptr);
	}
}

} // namespace

void generate_chunk(const Plan &p_plan, uint32_t p_chunk, ChunkBuffers &r_out, bool p_parallel) {
	const ChunkPlan &cp = p_plan.chunks[p_chunk];
	r_out.gaussians.resize(cp.count);
	r_out.sh_high.resize(cp.count * p_plan.sh_high_order);

	// Stratified merge of the parts: part j's k-th splat has rank (2k + 1) / (2 n_j);
	// ties go to the lower part. Every prefix keeps each part's share.
	LocalVector<uint32_t> order;
	order.resize(cp.count);
	uint32_t heads[256] = {};
	const uint32_t parts = MIN(cp.part_count, 256u);
	for (uint32_t i = 0; i < cp.count; i++) {
		uint32_t best = UINT32_MAX;
		for (uint32_t j = 0; j < parts; j++) {
			const uint32_t n = p_plan.parts[cp.first_part + j].count;
			if (heads[j] >= n) {
				continue;
			}
			if (best == UINT32_MAX) {
				best = j;
				continue;
			}
			const uint32_t nb = p_plan.parts[cp.first_part + best].count;
			const uint64_t lhs = (2u * uint64_t(heads[j]) + 1u) * nb;
			const uint64_t rhs = (2u * uint64_t(heads[best]) + 1u) * n;
			if (lhs < rhs) {
				best = j;
			}
		}
		order[i] = (best << 16u) | heads[best];
		heads[best]++;
	}

	ChunkJob job;
	job.plan = &p_plan;
	job.cp = &cp;
	job.order = order.ptr();
	job.g = r_out.gaussians.ptr();
	job.sh = r_out.sh_high.ptr();
	job.count = cp.count;
	const uint32_t blocks = (cp.count + kSplatsPerTask - 1u) / kSplatsPerTask;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (p_parallel && pool != nullptr && pool->get_thread_count() > 1 && blocks > 1u) {
		const WorkerThreadPool::GroupID gid = pool->add_native_group_task(&chunk_task, &job, int(blocks), -1, true, "gs_world_generator chunk");
		pool->wait_for_group_task_completion(gid);
	} else {
		for (uint32_t b = 0; b < blocks; b++) {
			chunk_task(&job, b);
		}
	}

	// Chunk-table geometry: AABB of centres +- 3 sigma (3 sigma is exact in float), the
	// float midpoint, and a radius rounded up on a 1/1024 m grid via an integer sqrt.
	Vector3 mn(INFINITY, INFINITY, INFINITY);
	Vector3 mx(-INFINITY, -INFINITY, -INFINITY);
	for (uint32_t i = 0; i < cp.count; i++) {
		const Gaussian &g = r_out.gaussians[i];
		const float r = 3.0f * MAX(MAX(g.scale.x, g.scale.y), g.scale.z);
		for (int a = 0; a < 3; a++) {
			mn[a] = MIN(mn[a], g.position[a] - r);
			mx[a] = MAX(mx[a], g.position[a] + r);
		}
	}
	r_out.desc.bounds = AABB(mn, mx - mn);
	Vector3 center;
	uint64_t r2 = 0;
	for (int a = 0; a < 3; a++) {
		center[a] = (mn[a] + mx[a]) * 0.5f;
		const double half = (double(mx[a]) - double(mn[a])) * 0.5;
		const uint64_t hq = uint64_t(std::ceil(half * 1024.0)) + 1u;
		r2 += hq * hq;
	}
	r_out.desc.center = center;
	r_out.desc.radius = float(double(isqrt_ceil(r2)) * (1.0 / 1024.0));
}

namespace {

struct ShaCtx {
	CryptoCore::SHA256Context ctx;
	ShaCtx() { ctx.start(); }
	void update(const void *p, uint64_t n) {
		const uint8_t *b = static_cast<const uint8_t *>(p);
		while (n > 0) {
			const size_t s = size_t(MIN(n, uint64_t(1) << 30u));
			ctx.update(b, s);
			b += s;
			n -= s;
		}
	}
	String finish() {
		unsigned char h[32];
		ctx.finish(h);
		return sha_hex(h);
	}
};

} // namespace

namespace {

// ---------------------------------------------------------------------------
// GSQ v1 read-back (plain v3 + bit 7; layout in gaussian_splat_world_format.h).
// ---------------------------------------------------------------------------

uint32_t le_u32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

uint64_t le_u64(const uint8_t *p) {
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

// One chunk of a paged world as read back from disk.
struct PagedChunkRead {
	float record_floats[10] = {}; // bounds position, bounds size, centre, radius
	uint32_t first_page = 0;
	uint32_t page_count = 0;
	uint32_t splat_count = 0;
	// The chunk's page run: from its first page's offset to its last page's end, the zero
	// padding between its pages included. Empty (0, 0) for a chunk without pages.
	uint64_t pages_offset = 0;
	uint64_t pages_bytes = 0;
	String pages_sha256;
};

struct PagedRead {
	uint8_t extension[gs_world_format::kPagedExtensionBytes] = {};
	uint32_t page_count = 0;
	uint64_t pages_stored_bytes = 0;
	String decoded_digest_set_sha256; // SHA-256 over every page's decoded digest, in page order
	String file_sha256;
	LocalVector<PagedChunkRead> chunks;
	// p_verify_pages: every page whose stored digest fails, as "page_invalid: chunk C page P: why".
	PackedStringArray page_problems;
};

// Reads a paged world sequentially once. Checks the table structure (contiguous page runs,
// aligned, ascending pages that end at the chunk table, zero padding, splat counts) and, with
// p_verify_pages, every page's stored digest against its bytes and its page-table record. A
// structural failure stops the read (an error); a page that fails its digest is listed in
// page_problems and the read goes on, so the caller still gets every page-run digest.
Error read_paged_world(const Ref<FileAccess> &p_file, uint64_t p_file_bytes, bool p_verify_pages, PagedRead &r_read, String &r_error) {
	constexpr uint64_t kHeaderBytes = gs_world_format::kPrefixBytes + gs_world_format::kPagedExtensionBytes;
	constexpr uint64_t kAlign = gs_world_format::kPagedPageAlignment;
	auto corrupt = [&](const String &p_msg) {
		r_error = p_msg;
		return ERR_FILE_CORRUPT;
	};
	uint8_t header[kHeaderBytes];
	p_file->seek(0);
	if (p_file_bytes < kHeaderBytes || p_file->get_buffer(header, kHeaderBytes) != kHeaderBytes) {
		return corrupt("paged: short read in the header");
	}
	if ((le_u32(header + 8) & gs_world_format::kFlagPagedChunks) == 0u) {
		return corrupt("paged: flag bit 7 is not set");
	}
	const uint32_t chunk_count = le_u32(header + 52);
	const uint64_t chunk_table_offset = le_u64(header + 72);
	const uint8_t *ext = header + gs_world_format::kPrefixBytes;
	memcpy(r_read.extension, ext, gs_world_format::kPagedExtensionBytes);
	const uint32_t page_count = le_u32(ext + 16);
	const uint64_t page_table_offset = le_u64(ext + 24);
	const uint64_t ct_bytes = uint64_t(chunk_count) * gs_world_format::kPagedChunkRecordBytes;
	const uint64_t pt_bytes = uint64_t(page_count) * gs_world_format::kPagedPageRecordBytes;
	if (le_u32(ext + 20) != kAlign || chunk_table_offset < kHeaderBytes || chunk_table_offset > p_file_bytes ||
			ct_bytes > p_file_bytes - chunk_table_offset || page_table_offset != chunk_table_offset + ct_bytes ||
			pt_bytes > p_file_bytes - page_table_offset || ct_bytes + pt_bytes > uint64_t(UINT32_MAX)) {
		return corrupt("paged: the chunk or page table lies outside the file");
	}
	LocalVector<uint8_t> tables;
	tables.resize(uint32_t(ct_bytes + pt_bytes));
	p_file->seek(chunk_table_offset);
	if (p_file->get_buffer(tables.ptr(), ct_bytes + pt_bytes) != ct_bytes + pt_bytes) {
		return corrupt("paged: short read in the chunk or page table");
	}
	const uint8_t *pt = tables.ptr() + ct_bytes;

	r_read.chunks.resize(chunk_count);
	uint32_t next_page = 0;
	uint64_t prev_end = kHeaderBytes;
	for (uint32_t c = 0; c < chunk_count; c++) {
		const uint8_t *rec = tables.ptr() + uint64_t(c) * gs_world_format::kPagedChunkRecordBytes;
		PagedChunkRead &out = r_read.chunks[c];
		memcpy(out.record_floats, rec, sizeof(out.record_floats));
		out.first_page = le_u32(rec + 40);
		out.page_count = le_u32(rec + 44);
		out.splat_count = le_u32(rec + 48);
		if (out.first_page != next_page || out.page_count > page_count - next_page || le_u32(rec + 52) != 0u) {
			return corrupt(vformat("paged: chunk %d's page run is not contiguous", c));
		}
		uint64_t splats = 0;
		for (uint32_t p = out.first_page; p < out.first_page + out.page_count; p++) {
			const uint8_t *e = pt + uint64_t(p) * gs_world_format::kPagedPageRecordBytes;
			const uint64_t off = le_u64(e);
			const uint32_t stored = le_u32(e + 8);
			if (le_u32(e + 104) != c || le_u32(e + 108) != 0u || off % kAlign != 0u || off < prev_end || stored == 0u ||
					off > chunk_table_offset || uint64_t(stored) > chunk_table_offset - off) {
				return corrupt(vformat("paged: page %d's record is inconsistent", p));
			}
			if (p == out.first_page) {
				out.pages_offset = off;
			}
			prev_end = off + stored;
			splats += le_u32(e + 12);
		}
		if (out.page_count > 0u) {
			out.pages_bytes = prev_end - out.pages_offset;
		}
		if (splats != out.splat_count) {
			return corrupt(vformat("paged: chunk %d's pages hold %s splats, its record %d", c, String::num_uint64(splats), out.splat_count));
		}
		next_page += out.page_count;
	}
	if (next_page != page_count) {
		return corrupt("paged: the page table holds pages of no chunk");
	}
	if (prev_end != chunk_table_offset) {
		return corrupt("paged: the pages do not end at the chunk table");
	}

	// One sequential pass: file digest, page-run digests, padding, page digests.
	p_file->seek(0);
	ShaCtx file_sha;
	ShaCtx decoded_set;
	LocalVector<uint8_t> buf;
	uint64_t pos = 0;
	auto read_into = [&](uint64_t n, ShaCtx *p_extra) -> bool {
		if (buf.size() < n) {
			buf.resize(uint32_t(n));
		}
		if (p_file->get_buffer(buf.ptr(), n) != n) {
			return false;
		}
		file_sha.update(buf.ptr(), n);
		if (p_extra != nullptr) {
			p_extra->update(buf.ptr(), n);
		}
		pos += n;
		return true;
	};
	// Reads up to p_to; with p_zero every byte read must be zero (the padding between pages).
	bool nonzero_padding = false;
	auto read_to = [&](uint64_t p_to, ShaCtx *p_extra, bool p_zero) -> bool {
		while (pos < p_to) {
			const uint64_t n = MIN(p_to - pos, kReadSlice);
			if (!read_into(n, p_extra)) {
				return false;
			}
			for (uint64_t i = 0; p_zero && i < n; i++) {
				nonzero_padding = nonzero_padding || buf[uint32_t(i)] != 0u;
			}
		}
		return true;
	};
	if (!read_to(kHeaderBytes, nullptr, false)) {
		return corrupt("paged: short read in the header");
	}
	for (uint32_t c = 0; c < chunk_count; c++) {
		PagedChunkRead &out = r_read.chunks[c];
		ShaCtx span;
		for (uint32_t p = out.first_page; p < out.first_page + out.page_count; p++) {
			const uint8_t *e = pt + uint64_t(p) * gs_world_format::kPagedPageRecordBytes;
			const uint32_t stored = le_u32(e + 8);
			if (!read_to(le_u64(e), p == out.first_page ? nullptr : &span, true) || !read_into(stored, &span)) {
				return corrupt(vformat("paged: short read in page %d", p));
			}
			if (p_verify_pages) {
				gs_gsq::PageInfo info;
				String reason;
				if (gs_gsq::verify_page(buf.ptr(), stored, info, reason) != OK) {
					r_read.page_problems.push_back(vformat("page_invalid: chunk %d page %d: %s", c, p, reason));
				} else if (memcmp(info.stored_digest, e + 16, gs_gsq::kDigestBytes) != 0) {
					r_read.page_problems.push_back(vformat("page_invalid: chunk %d page %d: the page-table stored digest differs from the page's", c, p));
				}
			}
			decoded_set.update(e + 48, gs_gsq::kDigestBytes);
			r_read.pages_stored_bytes += stored;
		}
		out.pages_sha256 = span.finish();
	}
	if (nonzero_padding) {
		return corrupt("paged: the padding between pages is not zero");
	}
	if (!read_to(p_file_bytes, nullptr, false)) {
		return corrupt("paged: short read in the tables or the metadata");
	}
	r_read.page_count = page_count;
	r_read.file_sha256 = file_sha.finish();
	r_read.decoded_digest_set_sha256 = decoded_set.finish();
	return OK;
}

} // namespace

Error compute_content_digest(const Params &p_params, String &r_hex, String &r_error) {
	Plan plan;
	const Error err = build_plan(p_params, plan, r_error);
	if (err != OK) {
		return err;
	}
	ShaCtx all;
	ChunkBuffers buf;
	for (uint32_t c = 0; c < plan.chunks.size(); c++) {
		generate_chunk(plan, c, buf);
		all.update(buf.gaussians.ptr(), uint64_t(buf.gaussians.size()) * sizeof(Gaussian));
		all.update(buf.sh_high.ptr(), uint64_t(buf.sh_high.size()) * sizeof(Vector3));
		all.update(&buf.desc.bounds, sizeof(AABB));
		all.update(&buf.desc.center, sizeof(Vector3));
		all.update(&buf.desc.radius, sizeof(float));
	}
	r_hex = all.finish();
	return OK;
}

// ---------------------------------------------------------------------------
// World + manifest.
// ---------------------------------------------------------------------------

Dictionary Report::to_dict() const {
	Dictionary d;
	d["ok"] = error == OK;
	d["error"] = int(error);
	d["message"] = message;
	d["world_path"] = world_path;
	d["manifest_path"] = manifest_path;
	d["generator_params_sha"] = params_sha;
	d["gen_sha"] = gen_sha;
	d["file_sha256"] = file_sha256;
	d["file_bytes"] = int64_t(file_bytes);
	d["splat_count"] = int64_t(splat_count);
	d["chunk_count"] = int64_t(chunk_count);
	d["plan_usec"] = int64_t(plan_usec);
	d["pass1_usec"] = int64_t(pass_usec[0]);
	d["pass2_usec"] = int64_t(pass_usec[1]);
	d["finalize_usec"] = int64_t(finalize_usec);
	d["verify_usec"] = int64_t(verify_usec);
	d["total_usec"] = int64_t(total_usec);
	d["codec_id"] = int64_t(codec_id);
	d["page_count"] = int64_t(page_count);
	d["pages_stored_bytes"] = int64_t(pages_stored_bytes);
	return d;
}

String manifest_path_for(const String &p_world_path) {
	return p_world_path.get_basename() + ".manifest.json";
}

namespace {

Array aabb_array(const AABB &p_box) {
	Array a;
	const Vector3 e = p_box.position + p_box.size;
	a.push_back(double(float(p_box.position.x)));
	a.push_back(double(float(p_box.position.y)));
	a.push_back(double(float(p_box.position.z)));
	a.push_back(double(float(e.x)));
	a.push_back(double(float(e.y)));
	a.push_back(double(float(e.z)));
	return a;
}

Array rect_array(const IRect &r) {
	Array a;
	a.push_back(r.x0);
	a.push_back(r.z0);
	a.push_back(r.x1);
	a.push_back(r.z1);
	return a;
}

Dictionary writer_block(const gs_world_writer::CodecParams &p_codec) {
	Dictionary w;
	w["target"] = "PLAIN_V3";
	w["source_mode"] = "REITERABLE";
	w["format_version"] = int64_t(gs_world_format::kVersion);
	w["gaussian_layout_version"] = int64_t(GAUSSIAN_STRUCT_LAYOUT_VERSION);
	w["gaussian_bytes"] = int64_t(sizeof(Gaussian));
	Dictionary codec;
	codec["codec_id"] = int64_t(p_codec.codec_id);
	Array sh_bits;
	for (int i = 0; i < 3; i++) {
		sh_bits.push_back(int64_t(p_codec.sh_bits[i]));
	}
	codec["sh_bits"] = sh_bits;
	codec["opacity_bits"] = int64_t(p_codec.opacity_bits);
	codec["pos_step_m"] = double(p_codec.pos_step_m);
	codec["zstd_level"] = int64_t(p_codec.zstd_level);
	w["codec"] = codec;
	return w;
}

Dictionary content_table(const Plan &plan, const LocalVector<uint32_t> &sel) {
	Dictionary t;
	uint64_t totals[CLASS_COUNT] = {};
	for (uint32_t c : sel) {
		for (int k = 0; k < CLASS_COUNT; k++) {
			totals[k] += plan.chunks[c].class_counts[k];
		}
	}
	uint64_t sum = 0;
	for (int k = 0; k < CLASS_COUNT; k++) {
		Dictionary row;
		row["splats"] = int64_t(totals[k]);
		row["instances"] = int64_t(plan.class_instances[k]);
		t[class_name(ContentClass(k))] = row;
		sum += totals[k];
	}
	t["total"] = int64_t(sum);
	t["plain_area_m2"] = plan.plain_area;
	return t;
}

Array features_array(const Plan &plan) {
	Array a;
	for (const Feature &f : plan.features) {
		Dictionary d;
		d["class"] = class_name(f.cls);
		d["index"] = int64_t(f.index);
		d["splats"] = f.count;
		d["footprint"] = rect_array(f.footprint);
		if (f.cls == CLASS_WALL) {
			d["plane_z_m"] = f.plane_z_m;
		} else if (f.cls == CLASS_BEACON) {
			d["id"] = int64_t(f.beacon_id);
			char buf[20];
			snprintf(buf, sizeof(buf), "%016" PRIx64, f.beacon_code);
			d["code_hex"] = String(buf);
			Array center;
			center.push_back(f.beacon_center_x_m);
			center.push_back(double(q12_to_float(plan.params.beacon_base_y_q + plan.params.beacon_panel_m * 2048)));
			center.push_back(f.beacon_center_z_m);
			d["center"] = center;
			Array normal;
			normal.push_back(0);
			normal.push_back(0);
			normal.push_back(1);
			d["normal"] = normal;
			d["panel_m"] = plan.params.beacon_panel_m;
		}
		a.push_back(d);
	}
	return a;
}

struct ChunkRecordOut {
	gs_world_writer::ChunkDesc desc;
	String gaussian_sha;
	String sh_sha;
};

} // namespace

Error generate_world(const Params &p_params, const String &p_world_path, const GenerateOptions &p_options, Report &r_report) {
	r_report = Report();
	r_report.world_path = p_world_path;
	const uint64_t t_start = OS::get_singleton()->get_ticks_usec();
	auto fail = [&](Error p_err, const String &p_msg) {
		r_report.error = p_err;
		r_report.message = "gs_world_generator: " + p_msg;
		r_report.total_usec = OS::get_singleton()->get_ticks_usec() - t_start;
		return p_err;
	};
	if (p_world_path.is_empty() || p_world_path.begins_with("res://")) {
		return fail(ERR_INVALID_PARAMETER, "the world path must be absolute or user://; generated worlds never enter res:// (ADR D-2)");
	}
	if (p_world_path.get_extension() != "gsplatworld") {
		return fail(ERR_INVALID_PARAMETER, "the world path must end in .gsplatworld");
	}

	Plan plan;
	String perr;
	Error err = build_plan(p_params, plan, perr);
	if (err != OK) {
		return fail(err, perr);
	}
	r_report.plan_usec = OS::get_singleton()->get_ticks_usec() - t_start;
	r_report.params_sha = plan.params_sha;

	LocalVector<uint32_t> sel;
	if (p_options.selection.is_empty()) {
		sel.resize(plan.chunks.size());
		for (uint32_t i = 0; i < sel.size(); i++) {
			sel[i] = i;
		}
	} else {
		for (uint32_t i = 0; i < p_options.selection.size(); i++) {
			const uint32_t c = p_options.selection[i];
			if (c >= plan.chunks.size() || (i > 0 && c <= p_options.selection[i - 1])) {
				return fail(ERR_INVALID_PARAMETER, "selection: chunk ids must be sorted, unique and inside the plan");
			}
			sel.push_back(c);
		}
	}
	uint64_t splats = 0;
	for (uint32_t c : sel) {
		splats += plan.chunks[c].count;
	}
	const bool is_ref = !p_options.selection.is_empty();
	if (is_ref && splats > p_options.max_selection_splats) {
		return fail(ERR_INVALID_PARAMETER, vformat("selection: %s splats exceed the T-ref bound %s (ADR §6.1)",
												   String::num_uint64(splats), String::num_uint64(p_options.max_selection_splats)));
	}
	Array sel_ids;
	if (is_ref) {
		for (uint32_t c : sel) {
			sel_ids.push_back(int64_t(c));
		}
	}
	const String selection_sha = is_ref ? sha256_of_string(canonical_json(sel_ids)) : String();

	if (p_options.codec_id != gs_world_writer::kCodecNone && p_options.codec_id != gs_world_writer::kCodecGsqV1) {
		return fail(ERR_INVALID_PARAMETER, vformat("codec_id %d is neither none (0) nor GSQ v1 (1)", p_options.codec_id));
	}
	const bool gsq = p_options.codec_id == gs_world_writer::kCodecGsqV1;
	const gs_world_writer::CodecParams codec = gsq ? gs_world_writer::gsq_v1_codec_params() : gs_world_writer::CodecParams();
	r_report.codec_id = codec.codec_id;
	Dictionary gen_inputs;
	gen_inputs["generator_id"] = kGeneratorId;
	gen_inputs["generator_version"] = int64_t(kGeneratorVersion);
	gen_inputs["generator_params_sha"] = plan.params_sha;
	gen_inputs["writer"] = writer_block(codec);
	r_report.gen_sha = sha256_of_string(canonical_json(gen_inputs));

	// A stale manifest must never describe the new world, even for a moment.
	const String manifest_path = manifest_path_for(p_world_path);
	if (FileAccess::exists(manifest_path)) {
		DirAccess::remove_absolute(manifest_path);
	}

	gs_world_writer::PlainSpec spec;
	spec.splat_count = uint32_t(splats);
	spec.sh_degree = uint32_t(p_params.sh_degree);
	spec.sh_first_order = plan.sh_first_order;
	spec.sh_high_order = plan.sh_high_order;
	spec.bounds = plan.world_bounds;
	spec.chunk_count = sel.size();
	spec.write_chunk_table = true;
	Dictionary gen_meta;
	gen_meta["generator_id"] = kGeneratorId;
	gen_meta["generator_version"] = int64_t(kGeneratorVersion);
	gen_meta["generator_params_sha"] = plan.params_sha;
	gen_meta["gen_sha"] = r_report.gen_sha;
	gen_meta["tier"] = p_params.tier;
	if (is_ref) {
		gen_meta["selection_sha"] = selection_sha;
	}
	spec.metadata["gs_generator"] = gen_meta;

	gs_world_writer::Writer writer;
	// The tier fixes the world's size; the writer's preflight still refuses a disk that cannot
	// hold it plus the default margin (BV-03b) before the first byte.
	err = writer.begin_plain(p_world_path, spec, codec, gs_world_writer::SourceMode::REITERABLE, gs_world_writer::Limits::unbounded());
	if (err != OK) {
		return fail(err, writer.get_error_message());
	}

	LocalVector<ChunkRecordOut> records;
	records.resize(sel.size());
	ChunkBuffers buf;
	const uint32_t passes = writer.get_pass_count();
	for (uint32_t pass = writer.current_pass(); pass != gs_world_writer::Writer::kNoPass; pass = writer.current_pass()) {
		const uint64_t t_pass = OS::get_singleton()->get_ticks_usec();
		int last_pct = -1;
		for (uint32_t i = 0; i < sel.size(); i++) {
			generate_chunk(plan, sel[i], buf);
			ChunkRecordOut &rec = records[i];
			if (pass == 1u) {
				rec.desc = buf.desc;
				if (!plan.world_bounds.encloses(buf.desc.bounds)) {
					writer.abort();
					return fail(ERR_BUG, vformat("chunk %d's AABB leaves the declared world bounds", sel[i]));
				}
				// GSQ is lossy: its read-back checks the pages, not the generated bytes.
				if (!gsq) {
					ShaCtx g;
					g.update(buf.gaussians.ptr(), uint64_t(buf.gaussians.size()) * sizeof(Gaussian));
					rec.gaussian_sha = g.finish();
				}
			} else {
				ShaCtx s;
				s.update(buf.sh_high.ptr(), uint64_t(buf.sh_high.size()) * sizeof(Vector3));
				rec.sh_sha = s.finish();
			}
			err = writer.append_chunk(buf.desc, buf.gaussians.ptr(), buf.sh_high.ptr(), uint32_t(buf.gaussians.size()));
			if (err != OK) {
				return fail(err, writer.get_error_message());
			}
			if (p_options.print_progress) {
				const int pct = int((uint64_t(i + 1u) * 20u) / sel.size()) * 5;
				if (pct != last_pct) {
					last_pct = pct;
					print_line(vformat("[GSWorldGenerator] pass %d/%d: %d%% (%d/%d chunks, %.1f s)", pass, passes, pct, i + 1u, sel.size(),
							double(OS::get_singleton()->get_ticks_usec() - t_pass) / 1e6));
				}
			}
		}
		r_report.pass_usec[pass - 1u] = OS::get_singleton()->get_ticks_usec() - t_pass;
	}
	const uint64_t t_fin = OS::get_singleton()->get_ticks_usec();
	err = writer.finalize();
	if (err != OK) {
		return fail(err, writer.get_error_message());
	}
	r_report.finalize_usec = OS::get_singleton()->get_ticks_usec() - t_fin;

	// Read-back verification: every digest is recomputed from the bytes on disk.
	const uint64_t t_ver = OS::get_singleton()->get_ticks_usec();
	Ref<FileAccess> f = FileAccess::open(p_world_path, FileAccess::READ);
	if (f.is_null()) {
		return fail(ERR_FILE_CANT_OPEN, "read-back: cannot open the finished world");
	}
	const uint64_t file_bytes = f->get_length();
	if (file_bytes != writer.get_expected_file_size()) {
		return fail(ERR_FILE_CORRUPT, "read-back: the file size differs from the writer's layout");
	}
	PagedRead paged;
	if (gsq) {
		String perr;
		if (read_paged_world(f, file_bytes, p_options.verify_readback, paged, perr) != OK) {
			return fail(ERR_FILE_CORRUPT, "read-back: " + perr);
		}
		if (!paged.page_problems.is_empty()) {
			return fail(ERR_FILE_CORRUPT, "read-back: " + paged.page_problems[0]);
		}
		if (paged.chunks.size() != sel.size()) {
			return fail(ERR_FILE_CORRUPT, "read-back: the paged chunk table lists another chunk count");
		}
		for (uint32_t i = 0; i < sel.size(); i++) {
			if (paged.chunks[i].splat_count != plan.chunks[sel[i]].count) {
				return fail(ERR_FILE_CORRUPT, vformat("read-back: chunk %d's pages hold another splat count than generated", sel[i]));
			}
		}
		r_report.file_sha256 = paged.file_sha256;
		r_report.page_count = paged.page_count;
		r_report.pages_stored_bytes = paged.pages_stored_bytes;
	} else {
		ShaCtx file_sha;
		LocalVector<uint8_t> slice;
		slice.resize(uint32_t(kReadSlice));
		auto read_hash = [&](uint64_t n, ShaCtx *p_extra) -> bool {
			while (n > 0) {
				const uint64_t s = MIN(n, kReadSlice);
				if (f->get_buffer(slice.ptr(), s) != s) {
					return false;
				}
				file_sha.update(slice.ptr(), s);
				if (p_extra != nullptr) {
					p_extra->update(slice.ptr(), s);
				}
				n -= s;
			}
			return true;
		};
		if (!read_hash(gs_world_format::kPrefixBytes, nullptr)) {
			return fail(ERR_FILE_CORRUPT, "read-back: short read in the header");
		}
		const bool check = p_options.verify_readback;
		for (int section = 0; section < (plan.sh_high_order > 0u ? 2 : 1); section++) {
			for (uint32_t i = 0; i < sel.size(); i++) {
				const uint64_t n = uint64_t(plan.chunks[sel[i]].count) * (section == 0 ? sizeof(Gaussian) : plan.sh_high_order * sizeof(Vector3));
				ShaCtx chunk_sha;
				if (!read_hash(n, check ? &chunk_sha : nullptr)) {
					return fail(ERR_FILE_CORRUPT, "read-back: short read in a payload section");
				}
				if (check) {
					const String got = chunk_sha.finish();
					const String &want = section == 0 ? records[i].gaussian_sha : records[i].sh_sha;
					if (got != want) {
						return fail(ERR_FILE_CORRUPT, vformat("read-back: chunk %d %s digest differs from the generated bytes", sel[i], section == 0 ? "gaussian" : "SH"));
					}
				}
			}
		}
		if (!read_hash(file_bytes - f->get_position(), nullptr)) {
			return fail(ERR_FILE_CORRUPT, "read-back: short read in the tail");
		}
		r_report.file_sha256 = file_sha.finish();
	}
	f.unref();
	r_report.file_bytes = file_bytes;
	r_report.splat_count = splats;
	r_report.chunk_count = sel.size();
	r_report.verify_usec = OS::get_singleton()->get_ticks_usec() - t_ver;

	// Sidecar manifest (§4.1, §6.4, W8).
	Dictionary m;
	m["schema"] = kManifestSchema;
	Dictionary gen;
	gen["id"] = kGeneratorId;
	gen["version"] = int64_t(kGeneratorVersion);
	m["generator"] = gen;
	m["generator_params"] = params_to_dict(p_params);
	m["generator_params_sha"] = plan.params_sha;
	m["gen_sha"] = r_report.gen_sha;
	m["gen_sha_inputs"] = gen_inputs;
	m["writer"] = writer_block(codec);
	Dictionary world;
	world["file"] = p_world_path.get_file();
	world["file_bytes"] = int64_t(file_bytes);
	world["file_sha256"] = r_report.file_sha256;
	world["splat_count"] = int64_t(splats);
	world["chunk_count"] = int64_t(sel.size());
	world["sh_degree"] = int64_t(p_params.sh_degree);
	world["sh_first_order"] = int64_t(plan.sh_first_order);
	world["sh_high_order"] = int64_t(plan.sh_high_order);
	world["bounds"] = aabb_array(plan.world_bounds);
	world["extent_m"] = rect_array(plan.world);
	world["max_chunk_splats"] = int64_t(plan.max_chunk_count);
	m["world"] = world;
	if (is_ref) {
		Dictionary s;
		s["kind"] = "T-ref";
		s["parent_chunk_count"] = int64_t(plan.chunks.size());
		s["parent_splat_count"] = int64_t(plan.total_splats);
		s["chunk_ids"] = sel_ids;
		s["selection_sha"] = selection_sha;
		m["selection"] = s;
	}
	m["content_table"] = content_table(plan, sel);
	m["features"] = features_array(plan);
	Array chunks;
	const uint64_t g_base = gs_world_format::kPrefixBytes;
	const uint64_t sh_base = g_base + splats * sizeof(Gaussian);
	uint64_t first = 0;
	for (uint32_t i = 0; i < sel.size(); i++) {
		const ChunkPlan &cp = plan.chunks[sel[i]];
		Dictionary d;
		d["index"] = int64_t(i);
		d["plan_index"] = int64_t(sel[i]);
		d["cell"] = rect_array(cp.cell);
		d["morton"] = int64_t(cp.morton);
		d["count"] = int64_t(cp.count);
		Array cc;
		for (int k = 0; k < CLASS_COUNT; k++) {
			cc.push_back(int64_t(cp.class_counts[k]));
		}
		d["class_counts"] = cc;
		d["first_splat"] = int64_t(first);
		d["aabb"] = aabb_array(records[i].desc.bounds);
		Array center;
		center.push_back(double(float(records[i].desc.center.x)));
		center.push_back(double(float(records[i].desc.center.y)));
		center.push_back(double(float(records[i].desc.center.z)));
		d["center"] = center;
		d["radius"] = double(records[i].desc.radius);
		if (gsq) {
			const PagedChunkRead &pc = paged.chunks[i];
			d["first_page"] = int64_t(pc.first_page);
			d["page_count"] = int64_t(pc.page_count);
			d["pages_offset"] = int64_t(pc.pages_offset);
			d["pages_bytes"] = int64_t(pc.pages_bytes);
			d["pages_sha256"] = pc.pages_sha256;
		} else {
			d["gaussian_offset"] = int64_t(g_base + first * sizeof(Gaussian));
			d["gaussian_bytes"] = int64_t(uint64_t(cp.count) * sizeof(Gaussian));
			d["gaussian_sha256"] = records[i].gaussian_sha;
		}
		if (!gsq && plan.sh_high_order > 0u) {
			d["sh_offset"] = int64_t(sh_base + first * plan.sh_high_order * sizeof(Vector3));
			d["sh_bytes"] = int64_t(uint64_t(cp.count) * plan.sh_high_order * sizeof(Vector3));
			d["sh_sha256"] = records[i].sh_sha;
		}
		chunks.push_back(d);
		first += cp.count;
	}
	m["chunks"] = chunks;
	Array class_names;
	for (int k = 0; k < CLASS_COUNT; k++) {
		class_names.push_back(class_name(ContentClass(k)));
	}
	m["class_order"] = class_names;
	Dictionary digest_rules;
	if (gsq) {
		// BV-C7: the codec block of a GSQ world. Only integers and digests, so the manifest stays
		// byte-deterministic; bytes per splat is file_bytes / splat_count.
		const gs_gsq::PageBuildStats &stats = writer.get_page_stats();
		Dictionary cb;
		cb["codec_id"] = int64_t(codec.codec_id);
		cb["name"] = "gsq_v1";
		cb["container"] = "plain_v3_paged_chunks";
		cb["page_count"] = int64_t(paged.page_count);
		cb["page_alignment"] = int64_t(gs_world_format::kPagedPageAlignment);
		cb["pages_stored_bytes"] = int64_t(paged.pages_stored_bytes);
		cb["decoded_digest_set_sha256"] = paged.decoded_digest_set_sha256;
		cb["constant_groups"] = int64_t(stats.constant_groups);
		cb["heterogeneous_chunks"] = int64_t(stats.heterogeneous_chunks);
		m["codec"] = cb;
		digest_rules["pages_sha256"] = "sha256 of the file bytes [pages_offset, pages_offset + pages_bytes): the chunk's page run, the zero padding between its pages included";
		digest_rules["decoded_digest_set_sha256"] = "sha256 of the concatenated 32-byte decoded digests of the page table, in page order";
	} else {
		digest_rules["gaussian_sha256"] = "sha256 of the chunk's bytes in the gaussian section (codec none: stored == decoded)";
		digest_rules["sh_sha256"] = "sha256 of the chunk's bytes in the high-order SH section (codec none: stored == decoded)";
	}
	digest_rules["file_sha256"] = "sha256 of the whole world file";
	digest_rules["generator_params_sha"] = "sha256 of canonical_json(generator_params): sorted keys, no whitespace";
	digest_rules["gen_sha"] = "sha256 of canonical_json(gen_sha_inputs)";
	m["digest_rules"] = digest_rules;

	const CharString text = (canonical_json(m) + "\n").utf8();
	GSAtomicFileTransaction mtx;
	err = mtx.begin(manifest_path);
	if (err == OK) {
		err = mtx.append(reinterpret_cast<const uint8_t *>(text.get_data()), uint64_t(text.length()));
	}
	if (err == OK) {
		err = mtx.commit();
	}
	if (err != OK) {
		return fail(err, "manifest: " + mtx.get_error_message());
	}
	r_report.manifest_path = manifest_path;
	r_report.total_usec = OS::get_singleton()->get_ticks_usec() - t_start;
	return OK;
}

namespace {

int64_t dict_int(const Dictionary &p_d, const char *p_key, int64_t p_default = -1) {
	int64_t v = p_default;
	if (!variant_to_int(p_d.get(p_key, Variant()), v)) {
		return p_default;
	}
	return v;
}

// verify_world() for a GSQ v1 world (BV-C7): the page runs, every page's stored digest, the
// chunk records, the codec block and the extension's codec parameters against the manifest.
void verify_paged_world(const Ref<FileAccess> &p_file, uint64_t p_file_bytes, const Dictionary &p_manifest, Array &r_problems) {
	PagedRead pr;
	String err;
	if (read_paged_world(p_file, p_file_bytes, true, pr, err) != OK) {
		r_problems.push_back(err);
		return;
	}
	for (const String &problem : pr.page_problems) {
		r_problems.push_back(problem);
	}
	const Array chunks = p_manifest.get("chunks", Array());
	if (int64_t(pr.chunks.size()) != int64_t(chunks.size())) {
		r_problems.push_back("chunk_count_mismatch: the paged chunk table lists another number of chunks");
		return;
	}
	for (int i = 0; i < chunks.size(); i++) {
		const Dictionary c = chunks[i];
		const PagedChunkRead &pc = pr.chunks[uint32_t(i)];
		if (String(c.get("pages_sha256", "")) != pc.pages_sha256) {
			r_problems.push_back(vformat("chunk_digest_mismatch: chunk %d pages_sha256", i));
		}
		if (dict_int(c, "first_page") != int64_t(pc.first_page) || dict_int(c, "page_count") != int64_t(pc.page_count) ||
				dict_int(c, "pages_offset") != int64_t(pc.pages_offset) || dict_int(c, "pages_bytes") != int64_t(pc.pages_bytes)) {
			r_problems.push_back(vformat("chunk_pages_mismatch: chunk %d", i));
		}
		const Array aabb = c.get("aabb", Array());
		bool geom_ok = aabb.size() == 6;
		for (int a = 0; a < 3 && geom_ok; a++) {
			geom_ok = float(double(aabb[a])) == pc.record_floats[a] &&
					float(double(aabb[a + 3])) == float(pc.record_floats[a] + pc.record_floats[a + 3]);
		}
		if (dict_int(c, "count") != int64_t(pc.splat_count) || !geom_ok || float(double(c.get("radius", 0.0))) != pc.record_floats[9]) {
			r_problems.push_back(vformat("chunk_record_mismatch: chunk %d", i));
		}
	}
	const Dictionary cb = p_manifest.get("codec", Dictionary());
	if (dict_int(cb, "page_count") != int64_t(pr.page_count) || dict_int(cb, "pages_stored_bytes") != int64_t(pr.pages_stored_bytes) ||
			String(cb.get("decoded_digest_set_sha256", "")) != pr.decoded_digest_set_sha256) {
		r_problems.push_back("codec_block_mismatch: page count, stored bytes or decoded digest set");
	}
	// The W16 codec block in the extension must be the manifest's writer codec block.
	const Dictionary wc = Dictionary(p_manifest.get("writer", Dictionary())).get("codec", Dictionary());
	const Array sh_bits = wc.get("sh_bits", Array());
	bool params_ok = sh_bits.size() == 3 && dict_int(wc, "codec_id") == int64_t(le_u32(pr.extension + 0)) &&
			dict_int(wc, "opacity_bits") == int64_t(pr.extension[7]) && dict_int(wc, "zstd_level") == int64_t(int32_t(le_u32(pr.extension + 12)));
	for (int b = 0; b < 3 && params_ok; b++) {
		params_ok = int64_t(sh_bits[b]) == int64_t(pr.extension[4 + b]);
	}
	float pos_step;
	memcpy(&pos_step, pr.extension + 8, 4);
	if (!params_ok || float(double(wc.get("pos_step_m", -1.0))) != pos_step) {
		r_problems.push_back("codec_params_mismatch: the file's codec block differs from the manifest's writer codec");
	}
	if (pr.file_sha256 != String(Dictionary(p_manifest.get("world", Dictionary())).get("file_sha256", ""))) {
		r_problems.push_back("file_sha256_mismatch");
	}
}

} // namespace

Error verify_world(const String &p_world_path, Dictionary &r_report) {
	Array problems;
	r_report = Dictionary();
	auto done = [&]() {
		r_report["ok"] = problems.is_empty();
		r_report["problems"] = problems;
		return problems.is_empty() ? OK : ERR_FILE_CORRUPT;
	};
	const String manifest_path = manifest_path_for(p_world_path);
	const String text = FileAccess::get_file_as_string(manifest_path);
	if (text.is_empty()) {
		problems.push_back("manifest_missing: " + manifest_path);
		return done();
	}
	const Variant parsed = JSON::parse_string(text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		problems.push_back("manifest_unparseable");
		return done();
	}
	const Dictionary m = parsed;
	if (String(m.get("schema", "")) != kManifestSchema) {
		problems.push_back("manifest_schema");
		return done();
	}
	// generator_params_sha must be the sha of the recorded parameters, and they must
	// still be a valid parameter set.
	if (sha256_of_string(canonical_json(m.get("generator_params", Variant()))) != String(m.get("generator_params_sha", ""))) {
		problems.push_back("generator_params_sha_mismatch");
	}
	Params params;
	String perr;
	if (params_from_dict(m.get("generator_params", Dictionary()), params, perr) != OK) {
		problems.push_back("generator_params_invalid: " + perr);
	}
	if (sha256_of_string(canonical_json(m.get("gen_sha_inputs", Variant()))) != String(m.get("gen_sha", ""))) {
		problems.push_back("gen_sha_mismatch");
	}
	const Dictionary world = m.get("world", Dictionary());
	const Array chunks = m.get("chunks", Array());
	int64_t file_bytes = 0, splat_count = 0, chunk_count = 0, sh_high = 0;
	variant_to_int(world.get("file_bytes", Variant()), file_bytes);
	variant_to_int(world.get("splat_count", Variant()), splat_count);
	variant_to_int(world.get("chunk_count", Variant()), chunk_count);
	variant_to_int(world.get("sh_high_order", Variant()), sh_high);
	if (chunk_count != chunks.size()) {
		problems.push_back("chunk_count_mismatch: manifest lists a different number of chunks");
		return done();
	}
	// Content table: per-chunk class counts sum to the table and to the splat count.
	{
		const Dictionary table = m.get("content_table", Dictionary());
		int64_t per_class[CLASS_COUNT] = {};
		int64_t sum = 0;
		for (int i = 0; i < chunks.size(); i++) {
			const Dictionary c = chunks[i];
			const Array cc = c.get("class_counts", Array());
			int64_t n = 0, count = 0;
			for (int k = 0; k < CLASS_COUNT && k < cc.size(); k++) {
				int64_t v = 0;
				variant_to_int(cc[k], v);
				per_class[k] += v;
				n += v;
			}
			variant_to_int(c.get("count", Variant()), count);
			if (n != count) {
				problems.push_back(vformat("content_table: chunk %d class counts sum to %d, count is %d", i, n, count));
			}
			sum += count;
		}
		for (int k = 0; k < CLASS_COUNT; k++) {
			int64_t want = 0;
			variant_to_int(Dictionary(table.get(class_name(ContentClass(k)), Dictionary())).get("splats", Variant()), want);
			if (want != per_class[k]) {
				problems.push_back(vformat("content_table: %s is %d in the table, %d over the chunks", class_name(ContentClass(k)), want, per_class[k]));
			}
		}
		int64_t total = 0;
		variant_to_int(table.get("total", Variant()), total);
		if (total != sum || sum != splat_count) {
			problems.push_back("content_table: total, chunk sum and splat_count disagree");
		}
	}
	Ref<FileAccess> f = FileAccess::open(p_world_path, FileAccess::READ);
	if (f.is_null()) {
		problems.push_back("world_missing");
		return done();
	}
	if (int64_t(f->get_length()) != file_bytes) {
		problems.push_back(vformat("file_bytes_mismatch: %s on disk, %d in the manifest", String::num_uint64(f->get_length()), file_bytes));
		return done();
	}
	uint8_t header[gs_world_format::kPrefixBytes];
	if (f->get_buffer(header, sizeof(header)) != sizeof(header)) {
		problems.push_back("header_short");
		return done();
	}
	auto u32 = [&](int off) {
		uint32_t v;
		memcpy(&v, header + off, 4);
		return v;
	};
	auto u64 = [&](int off) {
		uint64_t v;
		memcpy(&v, header + off, 8);
		return v;
	};
	if (u32(0) != gs_world_format::kMagic || u32(4) != gs_world_format::kVersion) {
		problems.push_back("header_magic_or_version");
	}
	if (int64_t(u32(12)) != splat_count || int64_t(u32(52)) != chunk_count || int64_t(u32(24)) != sh_high) {
		problems.push_back("header_counts_mismatch");
	}
	if (u32(104) != GAUSSIAN_STRUCT_LAYOUT_VERSION) {
		problems.push_back("header_layout_version");
	}
	// BV-C7: the manifest's writer codec must be the file's encoding.
	const bool file_paged = (u32(8) & gs_world_format::kFlagPagedChunks) != 0u;
	const int64_t manifest_codec = dict_int(Dictionary(Dictionary(m.get("writer", Dictionary())).get("codec", Dictionary())), "codec_id");
	if (file_paged != (manifest_codec == int64_t(gs_world_writer::kCodecGsqV1))) {
		problems.push_back("codec_mismatch: the manifest's writer codec is not the file's encoding");
		return done();
	}
	if (file_paged) {
		verify_paged_world(f, uint64_t(file_bytes), m, problems);
		return done();
	}
	const uint64_t chunk_table_offset = u64(72);
	ShaCtx file_sha;
	file_sha.update(header, sizeof(header));
	LocalVector<uint8_t> slice;
	slice.resize(uint32_t(kReadSlice));
	bool io_ok = true;
	auto read_hash = [&](uint64_t n, ShaCtx *p_extra) {
		while (n > 0 && io_ok) {
			const uint64_t s = MIN(n, kReadSlice);
			if (f->get_buffer(slice.ptr(), s) != s) {
				io_ok = false;
				break;
			}
			file_sha.update(slice.ptr(), s);
			if (p_extra != nullptr) {
				p_extra->update(slice.ptr(), s);
			}
			n -= s;
		}
	};
	for (int section = 0; section < (sh_high > 0 ? 2 : 1) && io_ok; section++) {
		const char *key_bytes = section == 0 ? "gaussian_bytes" : "sh_bytes";
		const char *key_sha = section == 0 ? "gaussian_sha256" : "sh_sha256";
		for (int i = 0; i < chunks.size() && io_ok; i++) {
			const Dictionary c = chunks[i];
			int64_t n = 0;
			variant_to_int(c.get(key_bytes, Variant()), n);
			ShaCtx cs;
			read_hash(uint64_t(n), &cs);
			if (io_ok && cs.finish() != String(c.get(key_sha, ""))) {
				problems.push_back(vformat("chunk_digest_mismatch: chunk %d %s", i, key_sha));
			}
		}
	}
	// Chunk table: each record must carry the manifest's count and geometry.
	if (io_ok) {
		const uint64_t here = f->get_position();
		if (here != chunk_table_offset) {
			problems.push_back("layout: the payload sections do not end at the chunk table");
		}
		for (int i = 0; i < chunks.size() && io_ok; i++) {
			uint8_t rec[gs_world_format::kChunkRecordBytes];
			if (f->get_buffer(rec, sizeof(rec)) != sizeof(rec)) {
				io_ok = false;
				break;
			}
			file_sha.update(rec, sizeof(rec));
			const Dictionary c = chunks[i];
			int64_t count = 0;
			variant_to_int(c.get("count", Variant()), count);
			uint32_t rc;
			memcpy(&rc, rec + 48, 4);
			float vals[6];
			memcpy(vals, rec, 24);
			const Array aabb = c.get("aabb", Array());
			bool geom_ok = aabb.size() == 6;
			for (int a = 0; a < 3 && geom_ok; a++) {
				geom_ok = float(double(aabb[a])) == vals[a] && float(double(aabb[a + 3])) == float(vals[a] + vals[a + 3]);
			}
			float radius;
			memcpy(&radius, rec + 36, 4);
			if (int64_t(rc) != count || !geom_ok || float(double(c.get("radius", 0.0))) != radius) {
				problems.push_back(vformat("chunk_record_mismatch: chunk %d", i));
			}
		}
		read_hash(uint64_t(file_bytes) - f->get_position(), nullptr);
	}
	if (!io_ok) {
		problems.push_back("short_read");
		return done();
	}
	if (file_sha.finish() != String(world.get("file_sha256", ""))) {
		problems.push_back("file_sha256_mismatch");
	}
	return done();
}

} // namespace gs_world_generator
