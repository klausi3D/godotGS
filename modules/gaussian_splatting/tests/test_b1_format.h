#pragma once

// B1 contracts use byte offsets so the tests-first patch compiles on layout 1.
#include "test_gaussian_layout_fingerprint.h"
#include "test_gs_world_writer_v4.h"
#include "test_hlod_d2_auto_tree.h"
#include "gs_gsq_codec_test_fixture.h"
#include "../persistence/gaussian_scene_payload.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../core/streaming_quantization.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_semantic.h"
#include "../io/gs_payload_page_store.h"
#include "core/io/compression.h"

namespace TestB1Format {
constexpr uint32_t WORD = 0x80ad1234u;
inline void set_word(Gaussian &g, uint32_t word) { memcpy(reinterpret_cast<uint8_t *>(&g) + 116, &word, 4); }
inline uint32_t word(const Gaussian &g) {
    uint32_t result;
    memcpy(&result, reinterpret_cast<const uint8_t *>(&g) + 116, 4);
    return result;
}
inline Ref< ::GaussianData> data(uint32_t value) {
    Ref< ::GaussianData> result = TestGaussianLayoutFingerprint::make_data();
    Vector<Gaussian> records;
    for (int i = 0; i < 2; ++i) {
        Gaussian g = result->get_gaussian(i);
        set_word(g, value);
        records.push_back(g);
    }
    result->set_gaussians(records);
    return result;
}
struct ErrorProbe {
    ErrorHandlerList handler;
    String messages;
    static void record(void *p_user, const char *, const char *, int, const char *p_error, const char *p_message, bool, ErrorHandlerType) {
        ErrorProbe *probe = static_cast<ErrorProbe *>(p_user);
        probe->messages += String(p_error) + " " + String(p_message);
    }
    ErrorProbe() { handler.errfunc = record; handler.userdata = this; add_error_handler(&handler); }
    ~ErrorProbe() { remove_error_handler(&handler); }
};
inline void sha(const uint8_t *p_data, uint64_t p_size, uint8_t *r_digest) {
    CryptoCore::SHA256Context hash;
    hash.start(); hash.update(p_data, p_size); hash.finish(r_digest);
}
inline void seal_v4(PackedByteArray &p_bytes) {
    uint8_t *b = p_bytes.ptrw();
    for (uint32_t i = 0; i < TestGSWorldWriterV4::kNodes; ++i) {
        const uint64_t index = 4096 + 64 + i * 256;
        const uint64_t offset = decode_uint64(b + index + 128);
        const uint64_t size = decode_uint64(b + index + 136);
        sha(b + offset, size, b + index + 192);
    }
    sha(b + 4096 + 64, TestGSWorldWriterV4::kNodes * 256, b + 4096 + 32);
    sha(b + 4096 + 32, 32, b + 176);
    sha(b, 4064, b + 4064);
}
inline bool write_bytes(const String &p_path, const PackedByteArray &p_bytes) {
    Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
    if (f.is_null()) { FAIL("open malformed fixture"); return false; }
    f->store_buffer(p_bytes.ptr(), p_bytes.size());
    return f->get_error() == OK;
}
inline Error read_store(GaussianSplatHlodPagedSource &p_source, Vector<Gaussian> &r_g, String &r_reason) {
    PayloadPageStore store;
    PayloadPageStore::Config config;
    config.admission_policy = PayloadPageStore::ADMIT_RETAINED_BYTES;
    Error error = store.start(config);
    if (error != OK) { FAIL("start store"); return error; }
    error = p_source.install_in_page_store(store, 1);
    if (error != OK) { FAIL("install store"); return error; }
    p_source._test_set_runtime_full_payload_validation(false);
    Vector<Vector3> sh;
    return p_source.read_node_payload_from_store(store, 1, 1, 0, r_g, sh, r_reason);
}

}

TEST_CASE("[GaussianSplatting][WorldIO][B1] v3 layout2 preserves semantic and layout1 garbage is zero in resident and staged reads") {
    using namespace TestB1Format;
    using namespace TestGaussianLayoutFingerprint;
    const String path = fixture_path("b1", ".gsplatworld");
    Ref<GaussianSplatWorld> source = make_world(data(WORD));
    ResourceFormatSaverGaussianSplatWorld saver;
    if (saver.save(source, path) != OK) { FAIL("save v3 control"); return; }
    CHECK(read_u32_at(path, WORLD_LAYOUT_WORD_OFFSET) == 2u);
    ResourceFormatLoaderGaussianSplatWorld loader;
    for (bool legacy : { false, true }) {
        if (legacy && !write_u32_at(path, WORLD_LAYOUT_WORD_OFFSET, 1u)) { FAIL("patch legacy layout"); break; }
        Error error = FAILED;
        Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &error);
        if (loaded.is_null() || error != OK || loaded->get_gaussian_data().is_null()) { FAIL("resident load"); break; }
        CHECK(word(loaded->get_gaussian_data()->get_gaussian(0)) == (legacy ? 0u : WORD));
        Ref<Resource> res = loader.load(path, "", &error);
        Ref<GaussianSplatWorld> streamed = res;
        if (streamed.is_null() || streamed->get_chunk_payload_source().is_null()) { FAIL("staged load"); break; }
        LocalVector<Gaussian> captured;
        LocalVector<Vector3> sh;
        uint32_t first = 0, high = 0;
        Ref<StagedFileChunkPayloadSource> staged = streamed->get_chunk_payload_source();
        if (!staged->capture_chunk_snapshot(0, 2, captured, sh, first, high) || captured.size() != 2) { FAIL("staged capture"); break; }
        CHECK(word(captured[0]) == (legacy ? 0u : WORD));
        const uint32_t indices[] = { 1, 0 };
        if (!staged->capture_indexed_chunk_snapshot(indices, 2, captured, sh, first, high) || captured.size() != 2) { FAIL("indexed capture"); break; }
        CHECK(word(captured[0]) == (legacy ? 0u : WORD));
    }
    remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] GSF schema2 roundtrip retains semantic and schema1 never reads old bytes") {
    using namespace TestB1Format;
    using namespace GaussianSplatting;
    const String path = TestGaussianLayoutFingerprint::fixture_path("b1", ".gsf");
    GaussianSceneSerializer serializer;
    serializer.set_compression_type(CompressionType::NONE);
    Ref< ::GaussianData> source = data(WORD), restored;
    restored.instantiate();
    if (serializer.save_scene(path, source.ptr()) != OK) { FAIL("save GSF"); return; }
    if (serializer.load_scene(path, restored.ptr()) != OK || restored->get_count() != 2) { FAIL("load GSF"); return; }
    CHECK(word(restored->get_gaussian(0)) == WORD);
    ::GaussianData::SaveSnapshot snapshot;
    if (source->capture_save_snapshot(snapshot) != OK) { FAIL("snapshot"); return; }
    PackedByteArray payload;
    if (ScenePayload::encode(snapshot, payload) != OK) { FAIL("encode canonical payload"); return; }
    CHECK(decode_uint32(payload.ptr() + 4) == 2u);
    CHECK(decode_uint32(payload.ptr() + 8) == 136u);
    // Construct schema1 from actual schema2 records, removing only the new word.
    if (decode_uint32(payload.ptr() + 8) == 136u) {
        PackedByteArray old;
        old.resize(32 + 2 * 132);
        memcpy(old.ptrw(), payload.ptr(), 32);
        encode_uint32(1, old.ptrw() + 4);
        encode_uint32(132, old.ptrw() + 8);
        for (int i = 0; i < 2; ++i) { memcpy(old.ptrw() + 32 + i * 132, payload.ptr() + 32 + i * 136, 132); }
        LocalVector<Gaussian> records;
        LocalVector<Vector3> sh;
        uint32_t first = 0, high = 0;
        bool mode = false, aa = false;
        if (ScenePayload::decode(old, 2, UINT64_MAX, records, sh, first, high, mode, aa) != OK || records.size() != 2) { FAIL("schema1 compatibility"); return; }
        CHECK(word(records[0]) == 0u);
    }
    TestGaussianLayoutFingerprint::remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] GSQ2 semantic planes and digest and GSQ1 compatibility") {
    using namespace TestB1Format;
    using namespace gs_gsq;
    TestGSQCodec::Fixture src = TestGSQCodec::make_fixture(4100, 0, 3, 923);
    for (uint32_t i = 0; i < src.gaussians.size(); ++i) { set_word(src.gaussians[i], WORD + i); }
    EncodeParams params;
    LocalVector<uint8_t> page;
    String reason;
    if (encode_page(src.gaussians.ptr(), nullptr, 4100, 0, params, page, reason) != OK) { FAIL("encode page"); return; }
    CHECK(decode_uint16(page.ptr() + 4) == 2u);
    LocalVector<Gaussian> output;
    output.resize(4100);
    DecodeContext context;
    if (context.decode_range(page.ptr(), page.size(), 0, 4100, output.ptr(), nullptr, reason) != OK) { FAIL("decode page"); return; }
    CHECK(word(output[0]) == WORD);
    CHECK(word(output[4099]) == WORD + 4099);
    uint8_t a[32], b[32], old_a[32], old_b[32];
    decoded_digest(output.ptr(), nullptr, 4100, 0, a, 2);
    decoded_digest(output.ptr(), nullptr, 4100, 0, old_a, 1);
    set_word(output[4099], WORD ^ 0x10000u);
    decoded_digest(output.ptr(), nullptr, 4100, 0, b, 2);
    decoded_digest(output.ptr(), nullptr, 4100, 0, old_b, 1);
    CHECK(memcmp(a, b, 32) != 0);
    CHECK(memcmp(old_a, old_b, 32) == 0);
    params.codec_id = 1;
    // v1 cannot express semantic input: legacy test input is explicitly untagged.
    for (Gaussian &g : src.gaussians) { set_word(g, 0); }
    if (encode_page(src.gaussians.ptr(), nullptr, 4100, 0, params, page, reason) != OK) { FAIL("encode v1 control"); return; }
    CHECK(decode_uint16(page.ptr() + 4) == 1u);
    if (context.decode_range(page.ptr(), page.size(), 0, 4100, output.ptr(), nullptr, reason) != OK) { FAIL("decode v1"); return; }
    CHECK(word(output[4099]) == 0u);
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] reserved semantic bits have a named refusal and legacy ignores garbage") {
    using namespace TestB1Format;
    for (uint32_t bit : { 28u, 29u, 30u }) {
        Gaussian g;
        set_word(g, WORD | (1u << bit));
        String reason;
        CHECK_FALSE(gaussian_prepare_persisted_payload(&g, 1, 2, &reason));
        CHECK(reason == "semantic_reserved_bits");
        CHECK(gaussian_prepare_persisted_payload(&g, 1, 1, &reason));
        CHECK(word(g) == 0u);
    }
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] V4 carries semantic and node wind byte") {
    using namespace TestB1Format;
    using namespace TestGSWorldWriterV4;
    Paths paths;
    Fixture fixture;
    if (!paths.create() || !fixture.build()) { FAIL("V4 fixture"); return; }
    for (uint32_t i = 0; i < kNodes; ++i) {
        fixture.nodes[i].topology.max_wind_weight = 173;
        for (Gaussian &g : fixture.bodies[i].gaussians) { set_word(g, WORD); }
    }
    WriterV4 writer;
    if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths)) { FAIL("V4 write"); return; }
    if (writer.finalize(fixture.nodes) != OK) { FAIL("finalize V4"); return; }
    GaussianSplatHlodPagedSource source;
    if (!open_source(paths, source)) { return; }
    CHECK(source.get_nodes()[0].topology.max_wind_weight == 173u);
    Vector<Gaussian> records;
    Vector<Vector3> sh;
    String reason;
    if (source.read_node_payload(0, records, sh, reason) != OK || records.size() != kSplats) { FAIL("read V4"); return; }
    CHECK(word(records[0]) == WORD);
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] GPU raw and quantized records carry the word at pinned offsets") {
    using namespace TestB1Format;
    CHECK(gaussian_pack_semantic(0x1234, 0xad) == WORD);
    CHECK(gaussian_semantic_region(WORD) == 0x1234);
    CHECK(gaussian_semantic_wind_weight(WORD) == 0xad);
    CHECK(gaussian_semantic_valid(WORD));
    CHECK(gaussian_semantic_validate(WORD | 0x0f000000u));
    CHECK_FALSE(gaussian_semantic_validate(WORD | 0x10000000u));
    Gaussian g = data(WORD)->get_gaussian(0);
    PackedGaussian raw;
    PackedGaussianQuantized quantized;
    SHCompressionMetrics metrics;
    ChunkQuantizationInfo bounds;
    pack_gaussian(g, raw, metrics);
    pack_gaussian_quantized(g, bounds, 0x12345678u, quantized, metrics);
    uint32_t got_raw, got_quantized;
    memcpy(&got_raw, reinterpret_cast<const uint8_t *>(&raw) + 28, 4);
    memcpy(&got_quantized, reinterpret_cast<const uint8_t *>(&quantized) + 152, 4);
    CHECK(sizeof(raw) == 176u);
    CHECK(sizeof(quantized) == 160u);
    CHECK(got_raw == WORD);
    CHECK(got_quantized == WORD);
    CHECK(gs_quantized_chunk_index(quantized) == 0x12345678u);
}

TEST_CASE("[GaussianSplatting][HLOD][WorldIO][B1] auto-tree codec2 invalidates codec1 key") {
    using namespace TestHlodD2AutoTree;
    Fixture fixture;
    if (!fixture.create()) { FAIL("auto-tree fixture"); return; }
    Dictionary options = fixture.options();
    options["codec"] = "gsq";
    const String output = fixture.dir.path_join("b1.gsplatworld");
    const Dictionary report = GaussianSplatHlodAutoTree::build(fixture.input, output, options);
    if (int(report.get("error", FAILED)) != OK) { FAIL("auto-tree build: ", report); return; }
    const Dictionary manifest = json_file(gs_world_generator::manifest_path_for(output));
    Dictionary old = manifest.duplicate(true);
    Dictionary codec = old.get("codec", Dictionary());
    CHECK(int64_t(codec.get("codec_id", 0)) == 2);
    codec["codec_id"] = 1;
    old["codec"] = codec;
    CHECK(String(manifest.get("tree_sha", "")) == tree_sha(manifest));
    CHECK(tree_sha(old) != tree_sha(manifest));
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] v3 and GSF loaders refuse reserved semantic bits by name") {
    using namespace TestB1Format;
    using namespace TestGaussianLayoutFingerprint;
    const String path = fixture_path("b1_reserved", ".gsplatworld");
    ResourceFormatSaverGaussianSplatWorld saver;
    if (saver.save(make_world(data(WORD)), path) != OK) { FAIL("save v3"); return; }
    const uint64_t offset = read_u64_at(path, 56);
    if (!write_u32_at(path, offset + 116, WORD | (1u << 29))) { FAIL("corrupt semantic"); return; }
    ErrorProbe probe;
    ResourceFormatLoaderGaussianSplatWorld loader;
    Error error = OK;
    const Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &error);
    CHECK(loaded.is_null());
    CHECK(error == ERR_FILE_CORRUPT);
    CHECK(probe.messages.contains("semantic_reserved_bits"));
    probe.messages = String();
    Ref<Resource> resource = loader.load(path, "", &error);
    Ref<GaussianSplatWorld> streamed = resource;
    if (streamed.is_valid() && streamed->get_chunk_payload_source().is_valid()) {
        LocalVector<Gaussian> g;
        LocalVector<Vector3> sh;
        uint32_t first = 0, high = 0;
        CHECK_FALSE(streamed->get_chunk_payload_source()->capture_chunk_snapshot(0, 2, g, sh, first, high));
        CHECK(probe.messages.contains("semantic_reserved_bits"));
    } else { FAIL("plain v3 directory must open before lazy read"); }
    ::GaussianData::SaveSnapshot snapshot;
    Ref< ::GaussianData> input = data(WORD);
    if (input->capture_save_snapshot(snapshot) != OK) { FAIL("GSF snapshot"); return; }
    PackedByteArray payload;
    if (GaussianSplatting::ScenePayload::encode(snapshot, payload) != OK || payload.size() < 168) { FAIL("GSF payload"); return; }
    encode_uint32(WORD | (1u << 30), payload.ptrw() + 32 + 132);
    LocalVector<Gaussian> g;
    LocalVector<Vector3> sh;
    uint32_t first = 0, high = 0;
    bool mode = false, aa = false;
    probe.messages = String();
    CHECK(GaussianSplatting::ScenePayload::decode(payload, 2, UINT64_MAX, g, sh, first, high, mode, aa) == ERR_FILE_CORRUPT);
    CHECK(probe.messages.contains("semantic_reserved_bits"));
    remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] V4 layout1 garbage is zero and reserved bits fail in both readers") {
    using namespace TestB1Format;
    using namespace TestGSWorldWriterV4;
    Paths paths;
    Fixture fixture;
    if (!paths.create() || !fixture.build()) { FAIL("V4 fixture"); return; }
    WriterV4 writer;
    if (!begin(writer, paths, fixture) || !append_all(writer, fixture, paths) || writer.finalize(fixture.nodes) != OK) { FAIL("write V4"); return; }
    PackedByteArray original;
    if (!read_bytes(paths.final_path, original)) { return; }
    CHECK(decode_uint32(original.ptr() + 28) == 2u);
    for (bool legacy : { true, false }) {
        PackedByteArray bytes = original.duplicate();
        uint8_t *b = bytes.ptrw();
        const uint64_t record = decode_uint64(b + 4096 + 64 + 128);
        encode_uint32(legacy ? 1u : 2u, b + 28);
        encode_uint32(legacy ? 1u : 2u, b + record + 48);
        encode_uint32(0xf1234567u, b + record + 64 + 116);
        seal_v4(bytes);
        if (!TestB1Format::write_bytes(paths.final_path, bytes)) { return; }
        GaussianSplatHlodPagedSource source;
        if (!open_source(paths, source)) { return; }
        Vector<Gaussian> g;
        Vector<Vector3> sh;
        String reason;
        const Error offline = source.read_node_payload(0, g, sh, reason);
        if (legacy) {
            if (offline != OK || g.size() != kSplats) { FAIL("layout1 read"); return; }
            CHECK(word(g[0]) == 0u);
        } else { CHECK(offline == ERR_FILE_CORRUPT); CHECK(reason.contains("semantic_reserved_bits")); }
        const Error runtime = read_store(source, g, reason);
        if (legacy) {
            if (runtime != OK || g.size() != kSplats) { FAIL("layout1 runtime read"); return; }
            CHECK(word(g[0]) == 0u);
        } else { CHECK(runtime == ERR_FILE_CORRUPT); CHECK(reason.contains("semantic_reserved_bits")); }
    }
    for (uint32_t byte : { 120u, 121u }) {
        PackedByteArray bytes = original.duplicate();
        bytes.ptrw()[4096 + 64 + byte] = 1;
        // Layout1 is an old-format package: its former wind slot stays reserved.
        if (byte == 120) { encode_uint32(1, bytes.ptrw() + 28); }
        seal_v4(bytes);
        if (!TestB1Format::write_bytes(paths.final_path, bytes)) { return; }
        GaussianSplatHlodPagedSource source;
        GaussianSplatHlodTree tree;
        Dictionary metadata;
        String reason;
        CHECK(source.open(paths.final_path, tree, metadata, reason) == ERR_FILE_CORRUPT);
        CHECK(reason == "v4_nonzero_reserved_node_record");
    }
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] GSQ decoder refuses reserved semantic planes by name") {
    using namespace TestB1Format;
    using namespace gs_gsq;
    TestGSQCodec::Fixture fixture = TestGSQCodec::make_fixture(1, 0, 3, 11);
    set_word(fixture.gaussians[0], WORD);
    LocalVector<uint8_t> page;
    String reason;
    if (encode_page(fixture.gaussians.ptr(), nullptr, 1, 0, EncodeParams(), page, reason) != OK) { FAIL("encode"); return; }
    CHECK(decode_uint16(page.ptr() + 4) == 2u);
    uint8_t raw[256];
    const int64_t raw_size = Compression::decompress(raw, sizeof(raw), page.ptr() + 256, page.size() - 256);
    if (raw_size < 4) { FAIL("decompress fixture frame"); return; }
    encode_uint32(WORD | (1u << 28), raw + raw_size - 4);
    LocalVector<uint8_t> mutated;
    mutated.resize(256 + Compression::get_max_compressed_buffer_size(raw_size));
    memcpy(mutated.ptr(), page.ptr(), 256);
    const int64_t size = Compression::compress(mutated.ptr() + 256, raw, raw_size);
    if (size <= 0) { FAIL("compress malformed planes"); return; }
    mutated.resize(256 + size);
    encode_uint32(mutated.size(), mutated.ptr() + 12);
    encode_uint32(size, mutated.ptr() + 64);
    CryptoCore::SHA256Context hash;
    hash.start(); hash.update(mutated.ptr(), 32); hash.update(mutated.ptr() + 64, mutated.size() - 64); hash.finish(mutated.ptr() + 32);
    Gaussian output;
    DecodeContext decoder;
    CHECK(decoder.decode_range(mutated.ptr(), mutated.size(), 0, 1, &output, nullptr, reason) != OK);
    CHECK(reason == "semantic_reserved_bits");
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] V4 never serves a GSQ page under the other codec id") {
    using namespace TestB1Format;
    using namespace TestGSWorldWriterV4;
    for (uint32_t codec_id : { 1u, 2u }) {
        Paths paths;
        Fixture fixture;
        if (!paths.create() || !fixture.build()) { FAIL("fixture"); return; }
        CodecParams codec = gsq_v2_codec_params();
        CHECK(codec.codec_id == 2u);
        codec.codec_id = codec_id;
        WriterV4 writer;
        if (!begin(writer, paths, fixture, codec) || !append_all(writer, fixture, paths) || writer.finalize(fixture.nodes) != OK) { FAIL("write GSQ fixture"); return; }
        PackedByteArray bytes;
        if (!read_bytes(paths.final_path, bytes)) { return; }
        encode_uint32(codec_id == 1u ? 2u : 1u, bytes.ptrw() + 60);
        seal_v4(bytes);
        if (!TestB1Format::write_bytes(paths.final_path, bytes)) { return; }
        GaussianSplatHlodPagedSource source;
        if (!open_source(paths, source)) { return; }
        Vector<Gaussian> g;
        Vector<Vector3> sh;
        String reason;
        CHECK(source.read_node_payload(0, g, sh, reason) == ERR_FILE_CORRUPT);
        CHECK(reason == "v4_gsq_page_layout_mismatch");
        CHECK(read_store(source, g, reason) == ERR_FILE_CORRUPT);
        CHECK(reason == "v4_gsq_page_layout_mismatch");
    }
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] unsupported PLY and packed asset exports refuse semantic before writing") {
    using namespace TestB1Format;
    Ref< ::GaussianData> source = data(WORD);
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    CHECK(asset->populate_from_gaussian_data(source) == ERR_UNAVAILABLE);
    const String path = TestGaussianLayoutFingerprint::fixture_path("b1_export", ".ply");
    PackedByteArray sentinel;
    sentinel.resize(4);
    encode_uint32(0x12345678u, sentinel.ptrw());
    if (!TestB1Format::write_bytes(path, sentinel)) { return; }
    CHECK(source->save_to_file(path) == ERR_UNAVAILABLE);
    PackedByteArray actual;
    if (!TestGSWorldWriterV4::read_bytes(path, actual)) { return; }
    CHECK(actual == sentinel);
    TestGaussianLayoutFingerprint::remove_fixture(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][B1] GSF raw layout1 padding is ignored and the scene parser names reserved semantic bits") {
    using namespace TestB1Format;
    using namespace GaussianSplatting;
    const String path = TestGaussianLayoutFingerprint::fixture_path("b1_gsf_legacy", ".gsf");
    PackedByteArray bytes;
    bytes.resize(sizeof(GSF_V2_CODEC_0));
    memcpy(bytes.ptrw(), GSF_V2_CODEC_0, sizeof(GSF_V2_CODEC_0));
    bool patched = false;
    for (uint64_t at = 0; at + GSF_CHUNK_HEADER_SIZE <= uint64_t(bytes.size());) {
        const uint32_t size = decode_uint32(bytes.ptr() + at + 4);
        const uint64_t payload = at + GSF_CHUNK_HEADER_SIZE;
        if (payload + size > uint64_t(bytes.size())) { FAIL("captured chunk extent"); return; }
        if (decode_uint32(bytes.ptr() + at) == uint32_t(ChunkType::GAUSSIAN_DATA)) {
            if (size < 4 + 2 * sizeof(Gaussian)) { FAIL("captured raw records"); return; }
            encode_uint32(0xf1234567u, bytes.ptrw() + payload + 4 + 116);
            uint32_t checksum = 2166136261u;
            for (uint32_t i = 0; i < size; ++i) { checksum = (checksum ^ bytes[payload + i]) * 16777619u; }
            encode_uint32(checksum, bytes.ptrw() + at + 8);
            patched = true;
            break;
        }
        at = payload + size;
    }
    if (!patched || !TestB1Format::write_bytes(path, bytes)) { FAIL("patch legacy padding"); return; }
    GaussianSceneSerializer serializer;
    Ref< ::GaussianData> restored;
    restored.instantiate();
    if (serializer.load_scene(path, restored.ptr()) != OK || restored->get_count() != 2) { FAIL("load patched v2 capture"); return; }
    CHECK(word(restored->get_gaussian(0)) == 0u);
    serializer.set_enable_checksum(false);
    serializer.set_compression_type(CompressionType::NONE);
    Ref< ::GaussianData> input = data(WORD);
    if (serializer.save_scene(path, input.ptr()) != OK || !TestGSWorldWriterV4::read_bytes(path, bytes)) { FAIL("save current GSF"); return; }
    patched = false;
    for (uint64_t at = 0; at + GSF_CHUNK_HEADER_SIZE <= uint64_t(bytes.size());) {
        const uint32_t size = decode_uint32(bytes.ptr() + at + 4);
        const uint64_t payload = at + GSF_CHUNK_HEADER_SIZE;
        if (payload + size > uint64_t(bytes.size())) { FAIL("current chunk extent"); return; }
        if (decode_uint32(bytes.ptr() + at) == uint32_t(ChunkType::GAUSSIAN_DATA)) {
            if (size < 32 + 136) { FAIL("canonical record extent"); return; }
            encode_uint32(WORD | (1u << 28), bytes.ptrw() + payload + 32 + 132);
            patched = true;
            break;
        }
        at = payload + size;
    }
    if (!patched || !TestB1Format::write_bytes(path, bytes)) { FAIL("patch schema2 semantic"); return; }
    ErrorProbe probe;
    CHECK(serializer.load_scene(path, restored.ptr()) == ERR_FILE_CORRUPT);
    CHECK(probe.messages.contains("semantic_reserved_bits"));
    TestGaussianLayoutFingerprint::remove_fixture(path);
}
