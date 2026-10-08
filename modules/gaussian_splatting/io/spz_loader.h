#ifndef SPZ_LOADER_H
#define SPZ_LOADER_H

#include "core/io/file_access.h"
#include "core/io/compression.h"
#include "core/object/ref_counted.h"
#include "core/variant/variant.h"
#include "core/variant/dictionary.h"
#include "../core/gaussian_data.h"

/**
 * Loader adapter for the pinned official Niantic SPZ decoder.
 * Supports bounded legacy gzip/wrappers and v4 multi-stream Zstd containers,
 * full SH0-4, coordinate/SH conversion and antialiased training metadata.
 * Runtime DC stores C0 * raw SH-DC; the raster route adds the 0.5 bias.
 */
class SPZLoader : public RefCounted {
    GDCLASS(SPZLoader, RefCounted);

public:
    /**
     * @struct SPZHeader
     * @brief Normalized legacy header information (16 bytes); v4 container metadata is separate.
     */
    struct SPZHeader {
        uint32_t magic;           ///< Magic number: 0x5053474E ("NGSP" in little-endian)
        uint32_t version;         ///< Supported format version (1-4)
        uint32_t num_points;      ///< Number of Gaussian points
        uint8_t sh_degree;        ///< Spherical harmonics degree (0-4)
        uint8_t fractional_bits;  ///< Fixed-point precision for positions
        uint8_t flags;            ///< Feature flags (bit 0x1 = antialiased training)
        uint8_t reserved;         ///< Must be zero
    };

    static constexpr uint32_t SPZ_MAGIC = 0x5053474E;  // "NGSP" in little-endian
    static constexpr uint32_t SPZ_VERSION_1 = 1;
    static constexpr uint32_t SPZ_VERSION_2 = 2;
    static constexpr uint32_t SPZ_VERSION_3 = 3;
    static constexpr uint32_t SPZ_VERSION_4 = 4;

    // Flag bit definitions
    static constexpr uint8_t SPZ_FLAG_ANTIALIASED = 0x01;

private:
    SPZHeader header;
    uint8_t container_stream_count = 0;
    Ref<::GaussianData> gaussian_data;

    // Decompression helpers
    Error decompress_data(const PackedByteArray &p_compressed, PackedByteArray &r_decompressed,
            uint64_t p_max_decompressed_bytes, uint64_t p_expected_decompressed_bytes = 0);

protected:
    static void _bind_methods();

public:
    SPZLoader();
    ~SPZLoader();

    /**
     * @brief Loads an SPZ file from disk.
     * @param p_path Path to the SPZ file.
     * @return OK on success, or an error code.
     */
    Error load_file(const String &p_path);

    /**
     * @brief Returns the loaded Gaussian data.
     * @return Reference to the GaussianData resource.
     */
    Ref<::GaussianData> get_gaussian_data() const { return gaussian_data; }

    /**
     * @brief Returns the number of splats loaded.
     * @return Number of Gaussian splats.
     */
    int get_splat_count() const;

    /**
     * @brief Returns loading statistics.
     * @return Dictionary containing format info, splat count, and bounds.
     */
    Dictionary get_load_statistics() const;

    /**
     * @brief Returns the SPZ header information.
     * @return Copy of the parsed header.
     */
    SPZHeader get_header() const { return header; }

    /**
     * @brief Checks if the file at the given path is a valid SPZ file.
     * @param p_path Path to check.
     * @return true if the file has a valid SPZ magic number.
     */
    static bool is_spz_file(const String &p_path);
};

#endif // SPZ_LOADER_H
