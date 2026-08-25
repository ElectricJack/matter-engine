#include "terrain_collision_artifact.h"

#include "bake_mode.h"
#include "terrain_mesher.h"
#include "terrain_river_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace matter::terrain_collision {
namespace {

constexpr std::array<std::uint8_t, 8> kTileMagic = {
    'M', 'T', 'C', 'T', 'L', 'E', '0', '1'};
constexpr std::array<std::uint8_t, 8> kManifestMagic = {
    'M', 'T', 'C', 'M', 'L', 'E', '0', '1'};
constexpr std::uint32_t kTileFormatVersion = 1u;
constexpr std::uint32_t kManifestFormatVersion = 1u;
constexpr std::uint64_t kTileHeaderBytes = 144u;
constexpr std::uint64_t kTileFileDigestOffset = 136u;
constexpr std::uint64_t kManifestHeaderBytes = 48u;
constexpr std::uint64_t kManifestRecordBytes = 80u;
constexpr std::uint64_t kManifestTrailerBytes = 8u;
constexpr std::uint64_t kBoxCountLimit =
    static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

using Clock = std::chrono::steady_clock;

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool checked_add(std::uint64_t a, std::uint64_t b,
                 std::uint64_t& result) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
    result = a + b;
    return true;
}

bool checked_mul(std::uint64_t a, std::uint64_t b,
                 std::uint64_t& result) noexcept {
    if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    result = a * b;
    return true;
}

bool to_size(std::uint64_t value, std::size_t& result) noexcept {
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()))
        return false;
    result = static_cast<std::size_t>(value);
    return true;
}

bool to_streamsize(std::size_t value, std::streamsize& result) noexcept {
    if (value > static_cast<std::size_t>(
                    std::numeric_limits<std::streamsize>::max()))
        return false;
    result = static_cast<std::streamsize>(value);
    return true;
}

std::uint32_t float_bits(float value) noexcept {
    std::uint32_t result = 0u;
    static_assert(sizeof(result) == sizeof(value), "float must be 32-bit");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float bits_float(std::uint32_t bits) noexcept {
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

class HashWriter {
public:
    void u8(std::uint8_t value) noexcept {
        state_ ^= value;
        state_ *= kFnvPrime;
    }
    void u32(std::uint32_t value) noexcept {
        for (unsigned shift = 0u; shift != 32u; shift += 8u)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0u; shift != 64u; shift += 8u)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void i64(std::int64_t value) noexcept {
        u64(static_cast<std::uint64_t>(value));
    }
    void f32(float value) noexcept { u32(float_bits(value)); }
    std::uint64_t finish() const noexcept { return state_; }

private:
    std::uint64_t state_ = kFnvOffset;
};

class Writer {
public:
    void reserve(std::size_t count) { bytes.reserve(count); }
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0u; shift != 32u; shift += 8u)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0u; shift != 64u; shift += 8u)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f32(float value) { u32(float_bits(value)); }
    void raw(const std::uint8_t* values, std::size_t count) {
        bytes.insert(bytes.end(), values, values + count);
    }
    std::vector<std::uint8_t> bytes;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : current_(data), remaining_(size) {}

    bool u8(std::uint8_t& value) noexcept {
        if (remaining_ == 0u) return false;
        value = *current_++;
        --remaining_;
        return true;
    }
    bool u32(std::uint32_t& value) noexcept {
        value = 0u;
        for (unsigned shift = 0u; shift != 32u; shift += 8u) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) noexcept {
        value = 0u;
        for (unsigned shift = 0u; shift != 64u; shift += 8u) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool i32(std::int32_t& value) noexcept {
        std::uint32_t bits = 0u;
        if (!u32(bits)) return false;
        value = static_cast<std::int32_t>(bits);
        return true;
    }
    bool i64(std::int64_t& value) noexcept {
        std::uint64_t bits = 0u;
        if (!u64(bits)) return false;
        value = static_cast<std::int64_t>(bits);
        return true;
    }
    bool f32(float& value) noexcept {
        std::uint32_t bits = 0u;
        if (!u32(bits)) return false;
        value = bits_float(bits);
        return true;
    }
    bool raw(std::uint8_t* values, std::size_t count) noexcept {
        if (count > remaining_) return false;
        std::memcpy(values, current_, count);
        current_ += count;
        remaining_ -= count;
        return true;
    }
    std::size_t remaining() const noexcept { return remaining_; }

private:
    const std::uint8_t* current_ = nullptr;
    std::size_t remaining_ = 0u;
};

std::uint64_t digest_with_zero_range(const std::uint8_t* bytes,
                                     std::size_t size,
                                     std::size_t zero_offset,
                                     std::size_t zero_count) noexcept {
    std::uint64_t digest = kFnvOffset;
    for (std::size_t index = 0u; index != size; ++index) {
        const std::uint8_t byte =
            index >= zero_offset && index - zero_offset < zero_count
                ? 0u : bytes[index];
        digest ^= byte;
        digest *= kFnvPrime;
    }
    return digest;
}

void patch_u64(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint64_t value) {
    for (unsigned shift = 0u; shift != 64u; shift += 8u)
        bytes[offset + shift / 8u] =
            static_cast<std::uint8_t>(value >> shift);
}

bool same_float_bits(float a, float b) noexcept {
    return float_bits(a) == float_bits(b);
}

bool same_float3_bits(const Float3& a, const Float3& b) noexcept {
    return same_float_bits(a.x, b.x) && same_float_bits(a.y, b.y) &&
           same_float_bits(a.z, b.z);
}

bool expected_origin(const CanonicalDefinition& definition,
                     const SectorCoordinate& coordinate,
                     Float3& origin, std::string& error) {
    const double sector_size = static_cast<double>(definition.sector_size_m);
    const double values[] = {
        static_cast<double>(coordinate.x) * sector_size,
        static_cast<double>(coordinate.y) * sector_size,
        static_cast<double>(coordinate.z) * sector_size,
    };
    origin = {static_cast<float>(values[0]), static_cast<float>(values[1]),
              static_cast<float>(values[2])};
    if (!std::isfinite(values[0]) || !std::isfinite(values[1]) ||
        !std::isfinite(values[2]) || !std::isfinite(origin.x) ||
        !std::isfinite(origin.y) || !std::isfinite(origin.z))
        return fail(error, "terrain collision tile origin is not finite");
    return true;
}

std::uint64_t tile_key_for(const CanonicalDefinition& definition,
                           const SectorCoordinate& coordinate) noexcept {
    HashWriter hash;
    hash.u32(0x314b544dU);  // MTK1
    hash.u64(definition.source.field_hash);
    hash.u64(definition.source.overlay_hash);
    if (definition.source.bake_mode_salt != 0u)
        hash.u64(definition.source.bake_mode_salt);
    hash.u32(definition.source.mesher_semantic_version);
    hash.u32(definition.source.geometry_format_version);
    hash.f32(definition.sector_size_m);
    hash.f32(definition.cell_size_m);
    hash.u8(static_cast<std::uint8_t>(definition.rung));
    hash.i64(coordinate.x);
    hash.i64(coordinate.y);
    hash.i64(coordinate.z);
    return hash.finish();
}

std::uint64_t tile_digest_for(const TileCandidate& tile) noexcept {
    HashWriter hash;
    hash.u32(0x3144544dU);  // MTD1
    hash.i64(tile.coordinate.x);
    hash.i64(tile.coordinate.y);
    hash.i64(tile.coordinate.z);
    hash.f32(tile.origin_m.x);
    hash.f32(tile.origin_m.y);
    hash.f32(tile.origin_m.z);
    hash.u64(static_cast<std::uint64_t>(tile.vertices.size()));
    for (const Float3& vertex : tile.vertices) {
        hash.f32(vertex.x);
        hash.f32(vertex.y);
        hash.f32(vertex.z);
    }
    hash.u64(static_cast<std::uint64_t>(tile.indices.size()));
    for (std::uint32_t index : tile.indices) hash.u32(index);
    return hash.finish();
}

bool coordinate_in_definition(const CanonicalDefinition& definition,
                              const SectorCoordinate& coordinate) {
    return std::binary_search(definition.sectors.begin(), definition.sectors.end(),
                              coordinate, sector_coordinate_less);
}

bool validate_definition_and_field(const terrain_field::FieldRuntime& field,
                                   const CanonicalDefinition& definition,
                                   std::string& error) {
    if (!std::isfinite(definition.sector_size_m) ||
        !(definition.sector_size_m > 0.0f) ||
        !std::isfinite(definition.cell_size_m) ||
        !(definition.cell_size_m > 0.0f))
        return fail(error, "terrain collision canonical dimensions are invalid");
    std::int8_t expected_rung_value = 0;
    if (!cell_size_to_rung(definition.cell_size_m, expected_rung_value) ||
        expected_rung_value != definition.rung)
        return fail(error, "terrain collision canonical rung is invalid");
    if (!std::isfinite(definition.friction) || definition.friction < 0.0f ||
        definition.friction > 1.0f ||
        !std::isfinite(definition.restitution) || definition.restitution < 0.0f ||
        definition.restitution > 1.0f)
        return fail(error, "terrain collision canonical material is invalid");
    if (definition.sectors.empty() ||
        definition.sectors.size() > kMaxSectorCount)
        return fail(error, "terrain collision canonical sector count is invalid");
    for (std::size_t index = 1u; index != definition.sectors.size(); ++index)
        if (!sector_coordinate_less(definition.sectors[index - 1u],
                                    definition.sectors[index]))
            return fail(error, "terrain collision canonical sectors are not sorted and unique");
    if (definition.source.mesher_semantic_version !=
            terrain_mesher::kSemanticVersion ||
        definition.source.geometry_format_version != kTileFormatVersion)
        return fail(error, "terrain collision source semantic version is stale");
    if (definition.source.bake_mode_salt != bake_mode::salt())
        return fail(error, "terrain collision source bake mode does not match the runtime");
    const bool has_overlay = static_cast<bool>(field.height_overlay());
    const std::uint64_t actual_overlay_hash = has_overlay
        ? field.height_overlay()->hash() : 0u;
    if (actual_overlay_hash != definition.source.overlay_hash)
        return fail(error, "terrain collision source overlay hash does not match the field");
    if (field.hash() != definition.source.field_hash)
        return fail(error, "terrain collision source field hash does not match the runtime");
    return true;
}

bool vertex_in_bounds(const Float3& vertex,
                      const CanonicalDefinition& definition) noexcept {
    const float lower = std::nextafter(-definition.cell_size_m,
                                       -std::numeric_limits<float>::infinity());
    const float upper = std::nextafter(definition.sector_size_m,
                                       std::numeric_limits<float>::infinity());
    return vertex.x >= lower && vertex.x <= upper &&
           vertex.y >= lower && vertex.y <= upper &&
           vertex.z >= lower && vertex.z <= upper;
}

struct TileHeader {
    std::uint32_t version = 0u;
    std::uint32_t mesher_semantic_version = 0u;
    std::uint32_t geometry_format_version = 0u;
    std::uint32_t reserved = 0u;
    std::uint64_t field_hash = 0u;
    std::uint64_t overlay_hash = 0u;
    std::uint64_t tile_key = 0u;
    SectorCoordinate coordinate{};
    Float3 origin_m{};
    float sector_size_m = 0.0f;
    float cell_size_m = 0.0f;
    std::int32_t rung = 0;
    std::uint64_t vertex_count = 0u;
    std::uint64_t index_count = 0u;
    std::uint64_t triangle_count = 0u;
    std::uint64_t payload_bytes = 0u;
    std::uint64_t tile_digest = 0u;
    std::uint64_t file_digest = 0u;
};

bool parse_tile_header(const std::uint8_t* bytes, std::size_t size,
                       const CanonicalDefinition& definition,
                       const SectorCoordinate& coordinate,
                       TileHeader& header, std::uint64_t& total_bytes,
                       std::string& error) {
    if (size < static_cast<std::size_t>(kTileHeaderBytes))
        return fail(error, "MTCT header is truncated");
    Reader reader(bytes, static_cast<std::size_t>(kTileHeaderBytes));
    std::array<std::uint8_t, 8> magic{};
    if (!reader.raw(magic.data(), magic.size()) || magic != kTileMagic ||
        !reader.u32(header.version) ||
        !reader.u32(header.mesher_semantic_version) ||
        !reader.u32(header.geometry_format_version) ||
        !reader.u32(header.reserved) || !reader.u64(header.field_hash) ||
        !reader.u64(header.overlay_hash) || !reader.u64(header.tile_key) ||
        !reader.i64(header.coordinate.x) || !reader.i64(header.coordinate.y) ||
        !reader.i64(header.coordinate.z) || !reader.f32(header.origin_m.x) ||
        !reader.f32(header.origin_m.y) || !reader.f32(header.origin_m.z) ||
        !reader.f32(header.sector_size_m) ||
        !reader.f32(header.cell_size_m) || !reader.i32(header.rung) ||
        !reader.u64(header.vertex_count) || !reader.u64(header.index_count) ||
        !reader.u64(header.triangle_count) ||
        !reader.u64(header.payload_bytes) ||
        !reader.u64(header.tile_digest) || !reader.u64(header.file_digest) ||
        reader.remaining() != 0u)
        return fail(error, "MTCT header is invalid");

    std::uint64_t vertex_bytes = 0u;
    std::uint64_t index_bytes = 0u;
    std::uint64_t payload_bytes = 0u;
    std::uint64_t expected_indices = 0u;
    if (!checked_mul(header.vertex_count, 12u, vertex_bytes) ||
        !checked_mul(header.index_count, 4u, index_bytes) ||
        !checked_add(vertex_bytes, index_bytes, payload_bytes) ||
        !checked_mul(header.triangle_count, 3u, expected_indices) ||
        !checked_add(kTileHeaderBytes, payload_bytes, total_bytes))
        return fail(error, "MTCT count arithmetic overflow");
    if (header.payload_bytes != payload_bytes ||
        header.index_count != expected_indices)
        return fail(error, "MTCT counts do not match the payload layout");
    if (header.vertex_count > kBoxCountLimit ||
        header.triangle_count > kBoxCountLimit)
        return fail(error, "MTCT counts exceed Box3D signed limits");
    std::size_t ignored_size = 0u;
    if (!to_size(header.vertex_count, ignored_size) ||
        !to_size(header.index_count, ignored_size) ||
        !to_size(total_bytes, ignored_size))
        return fail(error, "MTCT counts do not fit this platform");

    Float3 origin{};
    if (!expected_origin(definition, coordinate, origin, error)) return false;
    if (header.version != kTileFormatVersion || header.reserved != 0u ||
        header.mesher_semantic_version != terrain_mesher::kSemanticVersion ||
        header.mesher_semantic_version !=
            definition.source.mesher_semantic_version ||
        header.geometry_format_version !=
            definition.source.geometry_format_version ||
        header.field_hash != definition.source.field_hash ||
        header.overlay_hash != definition.source.overlay_hash ||
        header.tile_key != tile_key_for(definition, coordinate) ||
        !(header.coordinate == coordinate) ||
        !same_float3_bits(header.origin_m, origin) ||
        !same_float_bits(header.sector_size_m, definition.sector_size_m) ||
        !same_float_bits(header.cell_size_m, definition.cell_size_m) ||
        header.rung != definition.rung)
        return fail(error, "MTCT source, key, or coordinate is stale");
    return true;
}

bool serialize_tile(const TileCandidate& tile,
                    const CanonicalDefinition& definition,
                    std::vector<std::uint8_t>& bytes,
                    std::string& error) {
    bytes.clear();
    if (!detail::validate_tile_candidate(tile, definition, tile.coordinate, error))
        return false;
    std::uint64_t vertex_bytes = 0u;
    std::uint64_t index_bytes = 0u;
    std::uint64_t payload_bytes = 0u;
    std::uint64_t total_bytes = 0u;
    if (!checked_mul(static_cast<std::uint64_t>(tile.vertices.size()), 12u,
                     vertex_bytes) ||
        !checked_mul(static_cast<std::uint64_t>(tile.indices.size()), 4u,
                     index_bytes) ||
        !checked_add(vertex_bytes, index_bytes, payload_bytes) ||
        !checked_add(kTileHeaderBytes, payload_bytes, total_bytes))
        return fail(error, "MTCT serialization size overflow");
    std::size_t reserve_bytes = 0u;
    if (!to_size(total_bytes, reserve_bytes))
        return fail(error, "MTCT serialization is too large for this platform");

    Writer writer;
    writer.reserve(reserve_bytes);
    writer.raw(kTileMagic.data(), kTileMagic.size());
    writer.u32(kTileFormatVersion);
    writer.u32(terrain_mesher::kSemanticVersion);
    writer.u32(definition.source.geometry_format_version);
    writer.u32(0u);
    writer.u64(definition.source.field_hash);
    writer.u64(definition.source.overlay_hash);
    writer.u64(tile.tile_key);
    writer.i64(tile.coordinate.x);
    writer.i64(tile.coordinate.y);
    writer.i64(tile.coordinate.z);
    writer.f32(tile.origin_m.x);
    writer.f32(tile.origin_m.y);
    writer.f32(tile.origin_m.z);
    writer.f32(definition.sector_size_m);
    writer.f32(definition.cell_size_m);
    writer.i32(definition.rung);
    writer.u64(static_cast<std::uint64_t>(tile.vertices.size()));
    writer.u64(static_cast<std::uint64_t>(tile.indices.size()));
    writer.u64(static_cast<std::uint64_t>(tile.indices.size() / 3u));
    writer.u64(payload_bytes);
    writer.u64(tile.digest);
    writer.u64(0u);
    if (writer.bytes.size() != static_cast<std::size_t>(kTileHeaderBytes))
        return fail(error, "MTCT fixed header layout is inconsistent");
    for (const Float3& vertex : tile.vertices) {
        writer.f32(vertex.x);
        writer.f32(vertex.y);
        writer.f32(vertex.z);
    }
    for (std::uint32_t index : tile.indices) writer.u32(index);
    if (writer.bytes.size() != reserve_bytes)
        return fail(error, "MTCT serialization length is inconsistent");
    patch_u64(writer.bytes, static_cast<std::size_t>(kTileFileDigestOffset),
              digest_with_zero_range(
                  writer.bytes.data(), writer.bytes.size(),
                  static_cast<std::size_t>(kTileFileDigestOffset), 8u));
    bytes = std::move(writer.bytes);
    return true;
}

bool read_exact_file(const std::filesystem::path& path,
                     std::uint64_t expected_bytes,
                     std::vector<std::uint8_t>& bytes,
                     std::string& error,
                     std::uint32_t* open_error = nullptr) {
    std::size_t allocation_size = 0u;
    if (!to_size(expected_bytes, allocation_size))
        return fail(error, "terrain collision cache file is too large for this platform");
    std::streamsize read_size = 0;
    if (!to_streamsize(allocation_size, read_size))
        return fail(error, "terrain collision cache file exceeds stream limits");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
#ifdef _WIN32
        if (open_error) *open_error = static_cast<std::uint32_t>(GetLastError());
#endif
        return fail(error, "could not open terrain collision cache file");
    }
    bytes.resize(allocation_size);
    if (read_size != 0)
        stream.read(reinterpret_cast<char*>(bytes.data()), read_size);
    if (!stream || stream.gcount() != read_size)
        return fail(error, "could not read complete terrain collision cache file");
    return true;
}

bool load_tile_file(const std::filesystem::path& path,
                    const CanonicalDefinition& definition,
                    const SectorCoordinate& coordinate,
                    TileCandidate& tile, std::uint64_t& artifact_bytes,
                    double* validation_ms, std::string& error,
                    std::uint32_t* open_error = nullptr) {
    if (open_error) *open_error = 0u;
    std::error_code filesystem_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || file_size < kTileHeaderBytes ||
        file_size > std::numeric_limits<std::uint64_t>::max()) {
        if (open_error && filesystem_error)
            *open_error = static_cast<std::uint32_t>(filesystem_error.value());
        return fail(error, "MTCT file size is invalid");
    }
    std::ifstream header_stream(path, std::ios::binary);
    if (!header_stream) {
#ifdef _WIN32
        if (open_error) *open_error = static_cast<std::uint32_t>(GetLastError());
#endif
        return fail(error, "could not open MTCT header");
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kTileHeaderBytes)> header_bytes{};
    header_stream.read(reinterpret_cast<char*>(header_bytes.data()),
                       static_cast<std::streamsize>(header_bytes.size()));
    if (!header_stream || header_stream.gcount() !=
                              static_cast<std::streamsize>(header_bytes.size()))
        return fail(error, "could not read complete MTCT header");
    TileHeader header{};
    std::uint64_t expected_bytes = 0u;
    const Clock::time_point header_start = Clock::now();
    const bool header_valid = parse_tile_header(
        header_bytes.data(), header_bytes.size(), definition, coordinate, header,
        expected_bytes, error);
    if (validation_ms) *validation_ms += elapsed_ms(header_start);
    if (!header_valid) return false;
    if (expected_bytes != static_cast<std::uint64_t>(file_size))
        return fail(error, "MTCT exact file length does not match its counts");
    std::vector<std::uint8_t> bytes;
    if (!read_exact_file(path, expected_bytes, bytes, error, open_error))
        return false;
    const Clock::time_point validation_start = Clock::now();
    const bool valid = detail::validate_tile_artifact_bytes(
        bytes, definition, coordinate, tile, artifact_bytes, error);
    if (validation_ms) *validation_ms += elapsed_ms(validation_start);
    return valid;
}

std::string key_hex(std::uint64_t key) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << key;
    return stream.str();
}

std::filesystem::path tile_path_for(const std::filesystem::path& cache_root,
                                    std::uint64_t tile_key) {
    return cache_root / "terrain_collision" / "v1" / "tiles" /
           (key_hex(tile_key) + ".mtct");
}

std::filesystem::path manifest_path_for(
    const std::filesystem::path& cache_root, std::uint64_t installation_key) {
    return cache_root / "terrain_collision" / "v1" / "generations" /
           (key_hex(installation_key) + ".mtcm");
}

bool candidate_metadata_valid(const TerrainCollisionCandidate& candidate,
                              const CanonicalDefinition& definition,
                              std::string& error) {
    if (candidate.geometry_key != definition.geometry_key ||
        candidate.installation_key != definition.installation_key ||
        !same_float_bits(candidate.friction, definition.friction) ||
        !same_float_bits(candidate.restitution, definition.restitution) ||
        candidate.tiles.size() != definition.sectors.size())
        return fail(error, "terrain collision candidate identity is inconsistent");
    for (std::size_t index = 0u; index != candidate.tiles.size(); ++index)
        if (!detail::validate_tile_candidate(candidate.tiles[index], definition,
                                             definition.sectors[index], error))
            return false;
    return true;
}

bool serialize_manifest(const CanonicalDefinition& definition,
                        const TerrainCollisionCandidate& candidate,
                        const std::vector<std::uint64_t>& artifact_sizes,
                        std::vector<std::uint8_t>& bytes,
                        std::string& error) {
    bytes.clear();
    if (!candidate_metadata_valid(candidate, definition, error) ||
        artifact_sizes.size() != candidate.tiles.size())
        return artifact_sizes.size() == candidate.tiles.size()
            ? false : fail(error, "MTCM artifact-size table is inconsistent");
    std::uint64_t records_bytes = 0u;
    std::uint64_t total_bytes = 0u;
    if (!checked_mul(static_cast<std::uint64_t>(candidate.tiles.size()),
                     kManifestRecordBytes, records_bytes) ||
        !checked_add(kManifestHeaderBytes, records_bytes, total_bytes) ||
        !checked_add(total_bytes, kManifestTrailerBytes, total_bytes))
        return fail(error, "MTCM count arithmetic overflow");
    std::size_t reserve_bytes = 0u;
    if (!to_size(total_bytes, reserve_bytes))
        return fail(error, "MTCM is too large for this platform");

    Writer writer;
    writer.reserve(reserve_bytes);
    writer.raw(kManifestMagic.data(), kManifestMagic.size());
    writer.u32(kManifestFormatVersion);
    writer.u32(0u);
    writer.u64(candidate.geometry_key);
    writer.u64(candidate.installation_key);
    writer.f32(candidate.friction);
    writer.f32(candidate.restitution);
    writer.u64(static_cast<std::uint64_t>(candidate.tiles.size()));
    for (std::size_t index = 0u; index != candidate.tiles.size(); ++index) {
        const TileCandidate& tile = candidate.tiles[index];
        writer.i64(tile.coordinate.x);
        writer.i64(tile.coordinate.y);
        writer.i64(tile.coordinate.z);
        writer.u64(tile.tile_key);
        writer.u64(tile.digest);
        writer.u32(tile.indices.empty() ? 1u : 0u);
        writer.u32(0u);
        writer.u64(static_cast<std::uint64_t>(tile.vertices.size()));
        writer.u64(static_cast<std::uint64_t>(tile.indices.size()));
        writer.u64(static_cast<std::uint64_t>(tile.indices.size() / 3u));
        writer.u64(artifact_sizes[index]);
    }
    writer.u64(0u);
    if (writer.bytes.size() != reserve_bytes)
        return fail(error, "MTCM serialization length is inconsistent");
    const std::size_t digest_offset = writer.bytes.size() - 8u;
    patch_u64(writer.bytes, digest_offset,
              digest_with_zero_range(writer.bytes.data(), writer.bytes.size(),
                                     digest_offset, 8u));
    bytes = std::move(writer.bytes);
    return true;
}

bool parse_manifest_size(const std::uint8_t* bytes, std::size_t size,
                         const CanonicalDefinition& definition,
                         const TerrainCollisionCandidate& candidate,
                         std::uint64_t& total_bytes,
                         std::string& error) {
    if (size < static_cast<std::size_t>(kManifestHeaderBytes))
        return fail(error, "MTCM header is truncated");
    Reader reader(bytes, static_cast<std::size_t>(kManifestHeaderBytes));
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t version = 0u;
    std::uint32_t reserved = 0u;
    std::uint64_t geometry_key = 0u;
    std::uint64_t installation_key = 0u;
    float friction = 0.0f;
    float restitution = 0.0f;
    std::uint64_t tile_count = 0u;
    if (!reader.raw(magic.data(), magic.size()) || magic != kManifestMagic ||
        !reader.u32(version) || !reader.u32(reserved) ||
        !reader.u64(geometry_key) || !reader.u64(installation_key) ||
        !reader.f32(friction) || !reader.f32(restitution) ||
        !reader.u64(tile_count) || reader.remaining() != 0u)
        return fail(error, "MTCM header is invalid");
    std::uint64_t record_bytes = 0u;
    if (!checked_mul(tile_count, kManifestRecordBytes, record_bytes) ||
        !checked_add(kManifestHeaderBytes, record_bytes, total_bytes) ||
        !checked_add(total_bytes, kManifestTrailerBytes, total_bytes))
        return fail(error, "MTCM count arithmetic overflow");
    if (tile_count > kMaxSectorCount ||
        tile_count != static_cast<std::uint64_t>(candidate.tiles.size()))
        return fail(error, "MTCM tile count is invalid");
    std::size_t ignored_size = 0u;
    if (!to_size(total_bytes, ignored_size))
        return fail(error, "MTCM length does not fit this platform");
    if (version != kManifestFormatVersion || reserved != 0u ||
        geometry_key != definition.geometry_key ||
        installation_key != definition.installation_key ||
        !same_float_bits(friction, definition.friction) ||
        !same_float_bits(restitution, definition.restitution))
        return fail(error, "MTCM identity is stale");
    return true;
}

bool validate_manifest_bytes(const std::vector<std::uint8_t>& bytes,
                             const CanonicalDefinition& definition,
                             const TerrainCollisionCandidate& candidate,
                             const std::vector<std::uint64_t>& artifact_sizes,
                             std::string& error) {
    if (!candidate_metadata_valid(candidate, definition, error) ||
        artifact_sizes.size() != candidate.tiles.size())
        return artifact_sizes.size() == candidate.tiles.size()
            ? false : fail(error, "MTCM artifact-size table is inconsistent");
    std::uint64_t expected_bytes = 0u;
    if (!parse_manifest_size(bytes.data(), bytes.size(), definition, candidate,
                             expected_bytes, error) ||
        expected_bytes != static_cast<std::uint64_t>(bytes.size()))
        return expected_bytes == static_cast<std::uint64_t>(bytes.size())
            ? false : fail(error, "MTCM exact file length is invalid");
    const std::size_t digest_offset = bytes.size() - 8u;
    Reader digest_reader(bytes.data() + digest_offset, 8u);
    std::uint64_t stored_digest = 0u;
    if (!digest_reader.u64(stored_digest) ||
        stored_digest != digest_with_zero_range(
                             bytes.data(), bytes.size(), digest_offset, 8u))
        return fail(error, "MTCM whole-file digest mismatch");

    Reader reader(bytes.data() + static_cast<std::size_t>(kManifestHeaderBytes),
                  bytes.size() - static_cast<std::size_t>(kManifestHeaderBytes) - 8u);
    for (std::size_t index = 0u; index != candidate.tiles.size(); ++index) {
        SectorCoordinate coordinate{};
        std::uint64_t tile_key = 0u;
        std::uint64_t tile_digest = 0u;
        std::uint32_t empty = 0u;
        std::uint32_t reserved = 0u;
        std::uint64_t vertex_count = 0u;
        std::uint64_t index_count = 0u;
        std::uint64_t triangle_count = 0u;
        std::uint64_t artifact_bytes = 0u;
        if (!reader.i64(coordinate.x) || !reader.i64(coordinate.y) ||
            !reader.i64(coordinate.z) || !reader.u64(tile_key) ||
            !reader.u64(tile_digest) || !reader.u32(empty) ||
            !reader.u32(reserved) || !reader.u64(vertex_count) ||
            !reader.u64(index_count) || !reader.u64(triangle_count) ||
            !reader.u64(artifact_bytes))
            return fail(error, "MTCM record is truncated");
        const TileCandidate& tile = candidate.tiles[index];
        if (!(coordinate == tile.coordinate) || tile_key != tile.tile_key ||
            tile_digest != tile.digest || empty > 1u || reserved != 0u ||
            empty != (tile.indices.empty() ? 1u : 0u) ||
            vertex_count != static_cast<std::uint64_t>(tile.vertices.size()) ||
            index_count != static_cast<std::uint64_t>(tile.indices.size()) ||
            triangle_count != static_cast<std::uint64_t>(tile.indices.size() / 3u) ||
            artifact_bytes != artifact_sizes[index])
            return fail(error, "MTCM tile record does not match the validated tile");
    }
    if (reader.remaining() != 0u)
        return fail(error, "MTCM contains trailing record bytes");
    return true;
}

bool artifact_sizes_from_files(const std::filesystem::path& manifest_path,
                               const TerrainCollisionCandidate& candidate,
                               std::vector<std::uint64_t>& sizes,
                               std::string& error) {
    sizes.clear();
    sizes.reserve(candidate.tiles.size());
    const std::filesystem::path tile_directory =
        manifest_path.parent_path().parent_path() / "tiles";
    for (const TileCandidate& tile : candidate.tiles) {
        std::error_code filesystem_error;
        const std::uintmax_t size = std::filesystem::file_size(
            tile_directory / (key_hex(tile.tile_key) + ".mtct"), filesystem_error);
        if (filesystem_error || size > std::numeric_limits<std::uint64_t>::max())
            return fail(error, "MTCM references a missing or oversized tile artifact");
        sizes.push_back(static_cast<std::uint64_t>(size));
    }
    return true;
}

bool read_manifest_file(const std::filesystem::path& path,
                        const CanonicalDefinition& definition,
                        const TerrainCollisionCandidate& candidate,
                        const std::vector<std::uint64_t>& artifact_sizes,
                        std::vector<std::uint8_t>& bytes,
                        std::string& error,
                        std::uint32_t* open_error = nullptr) {
    if (open_error) *open_error = 0u;
    std::error_code filesystem_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || file_size <
            kManifestHeaderBytes + kManifestTrailerBytes ||
        file_size > std::numeric_limits<std::uint64_t>::max()) {
        if (open_error && filesystem_error)
            *open_error = static_cast<std::uint32_t>(filesystem_error.value());
        return fail(error, "MTCM file size is invalid");
    }
    std::ifstream header_stream(path, std::ios::binary);
    if (!header_stream) {
#ifdef _WIN32
        if (open_error) *open_error = static_cast<std::uint32_t>(GetLastError());
#endif
        return fail(error, "could not open MTCM header");
    }
    std::array<std::uint8_t, static_cast<std::size_t>(kManifestHeaderBytes)>
        header_bytes{};
    header_stream.read(reinterpret_cast<char*>(header_bytes.data()),
                       static_cast<std::streamsize>(header_bytes.size()));
    if (!header_stream || header_stream.gcount() !=
                              static_cast<std::streamsize>(header_bytes.size()))
        return fail(error, "could not read complete MTCM header");
    std::uint64_t expected_bytes = 0u;
    if (!parse_manifest_size(header_bytes.data(), header_bytes.size(), definition,
                             candidate, expected_bytes, error))
        return false;
    if (expected_bytes != static_cast<std::uint64_t>(file_size))
        return fail(error, "MTCM exact file length does not match its tile count");
    if (!read_exact_file(path, expected_bytes, bytes, error, open_error))
        return false;
    return validate_manifest_bytes(bytes, definition, candidate, artifact_sizes,
                                   error);
}

std::uint64_t process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path unique_temp_path(
    const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> serial{0u};
    const auto ticks = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    return destination.parent_path() /
        (destination.filename().string() + ".tmp-" +
         std::to_string(process_id()) + "-" + std::to_string(ticks) + "-" +
         std::to_string(serial.fetch_add(1u, std::memory_order_relaxed)));
}

void remove_temp(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool write_temp_file(const std::filesystem::path& destination,
                     const std::vector<std::uint8_t>& bytes,
                     std::filesystem::path& temp_path,
                     std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error)
        return fail(error, "could not create terrain collision cache directory");
    temp_path = unique_temp_path(destination);
    std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(error, "could not create terrain collision temporary file");
    std::streamsize write_size = 0;
    if (!to_streamsize(bytes.size(), write_size)) {
        stream.close();
        remove_temp(temp_path);
        return fail(error, "terrain collision temporary file exceeds stream limits");
    }
    if (write_size != 0)
        stream.write(reinterpret_cast<const char*>(bytes.data()), write_size);
    stream.flush();
    if (!stream) {
        stream.close();
        remove_temp(temp_path);
        return fail(error, "could not flush terrain collision temporary file");
    }
    stream.close();
    if (!stream) {
        remove_temp(temp_path);
        return fail(error, "could not close terrain collision temporary file");
    }
    return true;
}

#ifdef _WIN32
constexpr std::uint32_t kWindowsPublicationMaxAttempts = 4u;

DWORD move_file_windows(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    DWORD flags,
    detail::PublicationMoveOperation operation,
    const detail::BuildTestHooks* hooks) {
    DWORD move_error = ERROR_SUCCESS;
    for (std::uint32_t attempt = 0u;
         attempt != kWindowsPublicationMaxAttempts;
         ++attempt) {
        const std::uint32_t injected_error =
            hooks && hooks->publication_move_error
                ? hooks->publication_move_error(operation, attempt) : 0u;
        if (injected_error != 0u) {
            move_error = static_cast<DWORD>(injected_error);
        } else if (MoveFileExW(source.c_str(), destination.c_str(), flags) != 0) {
            return ERROR_SUCCESS;
        } else {
            move_error = GetLastError();
        }
        if (move_error != ERROR_ACCESS_DENIED) break;
    }
    return move_error;
}
#endif

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  const detail::BuildTestHooks* hooks,
                  std::string& error) {
#ifdef _WIN32
    if (move_file_windows(
            source, destination,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
            detail::PublicationMoveOperation::Replace, hooks) != ERROR_SUCCESS)
        return fail(error, "could not publish terrain collision cache file");
#else
    (void)hooks;
    if (std::rename(source.c_str(), destination.c_str()) != 0)
        return fail(error, "could not publish terrain collision cache file");
#endif
    return true;
}

enum class PublishNoReplaceResult {
    Published,
    DestinationExists,
    Failure,
};

PublishNoReplaceResult publish_file_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const detail::BuildTestHooks* hooks,
    std::string& error) {
#ifdef _WIN32
    const DWORD move_error = move_file_windows(
        source, destination, MOVEFILE_WRITE_THROUGH,
        detail::PublicationMoveOperation::NoReplace, hooks);
    if (move_error == ERROR_SUCCESS)
        return PublishNoReplaceResult::Published;
    if (move_error == ERROR_ALREADY_EXISTS || move_error == ERROR_FILE_EXISTS)
        return PublishNoReplaceResult::DestinationExists;
#else
    (void)hooks;
    if (::link(source.c_str(), destination.c_str()) == 0) {
        if (::unlink(source.c_str()) == 0)
            return PublishNoReplaceResult::Published;
        fail(error, "could not unlink published terrain collision temporary file");
        return PublishNoReplaceResult::Failure;
    }
    if (errno == EEXIST) return PublishNoReplaceResult::DestinationExists;
#endif
    fail(error, "could not publish terrain collision cache file");
    return PublishNoReplaceResult::Failure;
}

bool cancelled_now(const CancelCheck& cancelled) {
    return cancelled && cancelled();
}

bool continue_publication(
    const CancelCheck& cancelled,
    const detail::BuildTestHooks* hooks,
    detail::PublicationCommitPoint point,
    const std::filesystem::path& temp_path,
    std::string& error) {
    if (hooks && hooks->before_publication_commit)
        hooks->before_publication_commit(point);
    if (!cancelled_now(cancelled)) return true;
    remove_temp(temp_path);
    return fail(error, "terrain collision candidate build cancelled");
}

enum class WinnerValidationResult {
    Valid,
    Invalid,
    RetryableFailure,
};

WinnerValidationResult validate_tile_winner(
    const std::filesystem::path& destination,
    const CanonicalDefinition& definition,
    const SectorCoordinate& coordinate,
    TileCandidate& existing,
    std::uint64_t& existing_bytes,
    double& validation_ms,
    const detail::BuildTestHooks* hooks,
    std::string& existing_error) {
#ifdef _WIN32
    for (std::uint32_t attempt = 0u;
         attempt != kWindowsPublicationMaxAttempts;
         ++attempt) {
        std::uint32_t open_error =
            hooks && hooks->publication_winner_open_error
                ? hooks->publication_winner_open_error(
                      detail::PublicationWinnerArtifact::Tile, attempt) : 0u;
        if (open_error == 0u &&
            load_tile_file(destination, definition, coordinate, existing,
                           existing_bytes, &validation_ms, existing_error,
                           &open_error))
            return WinnerValidationResult::Valid;
        if (open_error != ERROR_SHARING_VIOLATION)
            return WinnerValidationResult::Invalid;
        existing_error = "could not open MTCT header";
    }
    return WinnerValidationResult::RetryableFailure;
#else
    (void)hooks;
    return load_tile_file(destination, definition, coordinate, existing,
                          existing_bytes, &validation_ms, existing_error)
        ? WinnerValidationResult::Valid : WinnerValidationResult::Invalid;
#endif
}

WinnerValidationResult validate_manifest_winner(
    const std::filesystem::path& destination,
    const CanonicalDefinition& definition,
    const TerrainCollisionCandidate& candidate,
    const std::vector<std::uint64_t>& artifact_sizes,
    std::vector<std::uint8_t>& existing_bytes,
    double& validation_ms,
    const detail::BuildTestHooks* hooks,
    std::string& existing_error) {
#ifdef _WIN32
    for (std::uint32_t attempt = 0u;
         attempt != kWindowsPublicationMaxAttempts;
         ++attempt) {
        std::uint32_t open_error =
            hooks && hooks->publication_winner_open_error
                ? hooks->publication_winner_open_error(
                      detail::PublicationWinnerArtifact::Manifest, attempt) : 0u;
        if (open_error == 0u) {
            const Clock::time_point start = Clock::now();
            const bool valid = read_manifest_file(
                destination, definition, candidate, artifact_sizes,
                existing_bytes, existing_error, &open_error);
            validation_ms += elapsed_ms(start);
            if (valid) return WinnerValidationResult::Valid;
        }
        if (open_error != ERROR_SHARING_VIOLATION)
            return WinnerValidationResult::Invalid;
        existing_error = "could not open MTCM header";
    }
    return WinnerValidationResult::RetryableFailure;
#else
    (void)hooks;
    const Clock::time_point start = Clock::now();
    const bool valid = read_manifest_file(
        destination, definition, candidate, artifact_sizes, existing_bytes,
        existing_error);
    validation_ms += elapsed_ms(start);
    return valid ? WinnerValidationResult::Valid
                 : WinnerValidationResult::Invalid;
#endif
}

bool publish_tile_file(const std::filesystem::path& destination,
                       const std::vector<std::uint8_t>& bytes,
                       const CanonicalDefinition& definition,
                       const SectorCoordinate& coordinate,
                       TileCandidate& tile, std::uint64_t& artifact_bytes,
                       double& validation_ms, const CancelCheck& cancelled,
                       const detail::BuildTestHooks* hooks,
                       std::string& error) {
    TileCandidate generated{};
    std::uint64_t generated_bytes = 0u;
    Clock::time_point start = Clock::now();
    if (!detail::validate_tile_artifact_bytes(
            bytes, definition, coordinate, generated, generated_bytes, error)) {
        validation_ms += elapsed_ms(start);
        return false;
    }
    validation_ms += elapsed_ms(start);
    std::filesystem::path temp_path;
    if (!write_temp_file(destination, bytes, temp_path, error)) return false;
    TileCandidate temp_tile{};
    std::uint64_t temp_bytes = 0u;
    if (!load_tile_file(temp_path, definition, coordinate, temp_tile, temp_bytes,
                        &validation_ms, error)) {
        remove_temp(temp_path);
        return false;
    }

    std::error_code filesystem_error;
    const bool destination_exists =
        std::filesystem::exists(destination, filesystem_error);
    if (filesystem_error) {
        remove_temp(temp_path);
        return fail(error, "could not inspect terrain collision tile destination");
    }
    if (destination_exists) {
        TileCandidate existing{};
        std::uint64_t existing_bytes = 0u;
        std::string existing_error;
        const WinnerValidationResult existing_result = validate_tile_winner(
            destination, definition, coordinate, existing, existing_bytes,
            validation_ms, hooks, existing_error);
        if (existing_result == WinnerValidationResult::Valid) {
            if (!continue_publication(
                    cancelled, hooks,
                    detail::PublicationCommitPoint::TileExistingWinner,
                    temp_path, error))
                return false;
            remove_temp(temp_path);
            tile = std::move(existing);
            artifact_bytes = existing_bytes;
            error.clear();
            return true;
        }
        if (existing_result == WinnerValidationResult::RetryableFailure) {
            remove_temp(temp_path);
            return fail(error,
                        "could not validate terrain collision tile winner");
        }
    } else {
        if (!continue_publication(
                cancelled, hooks,
                detail::PublicationCommitPoint::TileNoReplace,
                temp_path, error))
            return false;
        const PublishNoReplaceResult publish_result =
            publish_file_no_replace(temp_path, destination, hooks, error);
        if (publish_result == PublishNoReplaceResult::Published)
            return load_tile_file(destination, definition, coordinate, tile,
                                  artifact_bytes, &validation_ms, error);
        if (publish_result == PublishNoReplaceResult::Failure) {
            remove_temp(temp_path);
            return false;
        }
        TileCandidate existing{};
        std::uint64_t existing_bytes = 0u;
        std::string existing_error;
        const WinnerValidationResult existing_result = validate_tile_winner(
            destination, definition, coordinate, existing, existing_bytes,
            validation_ms, hooks, existing_error);
        if (existing_result == WinnerValidationResult::Valid) {
            if (!continue_publication(
                    cancelled, hooks,
                    detail::PublicationCommitPoint::TileExistingWinner,
                    temp_path, error))
                return false;
            remove_temp(temp_path);
            tile = std::move(existing);
            artifact_bytes = existing_bytes;
            error.clear();
            return true;
        }
        if (existing_result == WinnerValidationResult::RetryableFailure) {
            remove_temp(temp_path);
            return fail(error,
                        "could not validate terrain collision tile winner");
        }
    }
    if (!continue_publication(
            cancelled, hooks, detail::PublicationCommitPoint::TileReplace,
            temp_path, error))
        return false;
    if (!replace_file(temp_path, destination, hooks, error)) {
        remove_temp(temp_path);
        return false;
    }
    if (!load_tile_file(destination, definition, coordinate, tile, artifact_bytes,
                        &validation_ms, error))
        return false;
    return true;
}

bool publish_manifest_file(const std::filesystem::path& destination,
                           const std::vector<std::uint8_t>& bytes,
                           const CanonicalDefinition& definition,
                           const TerrainCollisionCandidate& candidate,
                           const std::vector<std::uint64_t>& artifact_sizes,
                           double& validation_ms, const CancelCheck& cancelled,
                           const detail::BuildTestHooks* hooks,
                           std::string& error) {
    Clock::time_point start = Clock::now();
    if (!validate_manifest_bytes(bytes, definition, candidate, artifact_sizes,
                                 error)) {
        validation_ms += elapsed_ms(start);
        return false;
    }
    validation_ms += elapsed_ms(start);
    std::filesystem::path temp_path;
    if (!write_temp_file(destination, bytes, temp_path, error)) return false;
    std::vector<std::uint8_t> temp_bytes;
    start = Clock::now();
    if (!read_manifest_file(temp_path, definition, candidate, artifact_sizes,
                            temp_bytes, error)) {
        validation_ms += elapsed_ms(start);
        remove_temp(temp_path);
        return false;
    }
    validation_ms += elapsed_ms(start);

    std::error_code filesystem_error;
    const bool destination_exists =
        std::filesystem::exists(destination, filesystem_error);
    if (filesystem_error) {
        remove_temp(temp_path);
        return fail(error, "could not inspect terrain collision manifest destination");
    }
    if (destination_exists) {
        std::vector<std::uint8_t> existing_bytes;
        std::string existing_error;
        const WinnerValidationResult existing_result = validate_manifest_winner(
            destination, definition, candidate, artifact_sizes, existing_bytes,
            validation_ms, hooks, existing_error);
        if (existing_result == WinnerValidationResult::Valid) {
            if (!continue_publication(
                    cancelled, hooks,
                    detail::PublicationCommitPoint::ManifestExistingWinner,
                    temp_path, error))
                return false;
            remove_temp(temp_path);
            error.clear();
            return true;
        }
        if (existing_result == WinnerValidationResult::RetryableFailure) {
            remove_temp(temp_path);
            return fail(error,
                        "could not validate terrain collision manifest winner");
        }
    } else {
        if (!continue_publication(
                cancelled, hooks,
                detail::PublicationCommitPoint::ManifestNoReplace,
                temp_path, error))
            return false;
        const PublishNoReplaceResult publish_result =
            publish_file_no_replace(temp_path, destination, hooks, error);
        if (publish_result == PublishNoReplaceResult::Published) {
            std::vector<std::uint8_t> published_bytes;
            start = Clock::now();
            const bool published = read_manifest_file(
                destination, definition, candidate, artifact_sizes,
                published_bytes, error);
            validation_ms += elapsed_ms(start);
            return published;
        }
        if (publish_result == PublishNoReplaceResult::Failure) {
            remove_temp(temp_path);
            return false;
        }
        std::vector<std::uint8_t> existing_bytes;
        std::string existing_error;
        const WinnerValidationResult existing_result = validate_manifest_winner(
            destination, definition, candidate, artifact_sizes, existing_bytes,
            validation_ms, hooks, existing_error);
        if (existing_result == WinnerValidationResult::Valid) {
            if (!continue_publication(
                    cancelled, hooks,
                    detail::PublicationCommitPoint::ManifestExistingWinner,
                    temp_path, error))
                return false;
            remove_temp(temp_path);
            error.clear();
            return true;
        }
        if (existing_result == WinnerValidationResult::RetryableFailure) {
            remove_temp(temp_path);
            return fail(error,
                        "could not validate terrain collision manifest winner");
        }
    }
    if (!continue_publication(
            cancelled, hooks, detail::PublicationCommitPoint::ManifestReplace,
            temp_path, error))
        return false;
    if (!replace_file(temp_path, destination, hooks, error)) {
        remove_temp(temp_path);
        return false;
    }
    std::vector<std::uint8_t> published_bytes;
    start = Clock::now();
    const bool published = read_manifest_file(
        destination, definition, candidate, artifact_sizes, published_bytes, error);
    validation_ms += elapsed_ms(start);
    return published;
}

bool add_stat(std::uint64_t value, std::uint64_t& total,
              std::string& error) {
    std::uint64_t result = 0u;
    if (!checked_add(total, value, result))
        return fail(error, "terrain collision candidate statistics overflow");
    total = result;
    return true;
}

bool load_or_build_candidate_impl(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    const detail::BuildTestHooks* hooks,
    TerrainCollisionCandidate& out,
    std::string& error) {
    out = {};
    error.clear();
    if (!validate_definition_and_field(field, definition, error)) return false;

    TerrainCollisionCandidate result{};
    result.geometry_key = definition.geometry_key;
    result.installation_key = definition.installation_key;
    result.friction = definition.friction;
    result.restitution = definition.restitution;
    result.tiles.reserve(definition.sectors.size());
    std::vector<std::uint64_t> artifact_sizes;
    artifact_sizes.reserve(definition.sectors.size());

    for (const SectorCoordinate& coordinate : definition.sectors) {
        if (cancelled_now(cancelled))
            return fail(error, "terrain collision candidate build cancelled");
        const std::uint64_t expected_key = tile_key_for(definition, coordinate);
        const std::filesystem::path path = tile_path_for(cache_root, expected_key);
        TileCandidate tile{};
        std::uint64_t tile_file_bytes = 0u;
        bool cache_hit = false;
        std::error_code filesystem_error;
        if (std::filesystem::exists(path, filesystem_error) && !filesystem_error) {
            const Clock::time_point load_start = Clock::now();
            std::string load_error;
            cache_hit = load_tile_file(path, definition, coordinate, tile,
                                       tile_file_bytes, &result.stats.validation_ms,
                                       load_error);
            result.stats.cache_load_ms += elapsed_ms(load_start);
        }
        if (cache_hit) {
            ++result.stats.cache_hit_tiles;
        } else {
            const Clock::time_point build_start = Clock::now();
            terrain_mesher::SectorMesh mesh{};
            std::string mesh_error;
            const bool mesh_ok = hooks && hooks->mesh_tile
                ? hooks->mesh_tile(field, coordinate, definition.rung,
                                   definition.sector_size_m, mesh, mesh_error)
                : terrain_mesher::mesh_sector_tiled(
                      field, coordinate.x, coordinate.y, coordinate.z,
                      definition.rung, definition.sector_size_m, mesh, nullptr,
                      mesh_error);
            if (!mesh_ok)
                return fail(error, "terrain collision mesher failed: " + mesh_error);
            if (cancelled_now(cancelled))
                return fail(error, "terrain collision candidate build cancelled");
            const Clock::time_point conversion_start = Clock::now();
            if (!detail::convert_mesh_to_tile(mesh, definition, coordinate, tile,
                                              error)) {
                result.stats.validation_ms += elapsed_ms(conversion_start);
                return false;
            }
            result.stats.validation_ms += elapsed_ms(conversion_start);
            std::vector<std::uint8_t> bytes;
            if (!serialize_tile(tile, definition, bytes, error)) return false;
            result.stats.cold_build_ms += elapsed_ms(build_start);
            if (cancelled_now(cancelled))
                return fail(error, "terrain collision candidate build cancelled");
            if (!publish_tile_file(path, bytes, definition, coordinate, tile,
                                   tile_file_bytes, result.stats.validation_ms,
                                   cancelled, hooks, error))
                return false;
            ++result.stats.built_tiles;
        }

        if (tile.vertices.empty() && tile.indices.empty())
            ++result.stats.empty_tiles;
        if (!add_stat(static_cast<std::uint64_t>(tile.indices.size() / 3u),
                      result.stats.triangle_count, error) ||
            !add_stat(static_cast<std::uint64_t>(tile.vertices.size()),
                      result.stats.unique_vertex_count, error) ||
            !add_stat(tile_file_bytes, result.stats.artifact_bytes, error))
            return false;
        artifact_sizes.push_back(tile_file_bytes);
        result.tiles.push_back(std::move(tile));
    }

    if (cancelled_now(cancelled))
        return fail(error, "terrain collision candidate build cancelled");
    const std::filesystem::path manifest =
        manifest_path_for(cache_root, definition.installation_key);
    std::error_code filesystem_error;
    bool manifest_valid = false;
    if (std::filesystem::exists(manifest, filesystem_error) && !filesystem_error) {
        std::vector<std::uint8_t> existing_bytes;
        const Clock::time_point validation_start = Clock::now();
        std::string manifest_error;
        manifest_valid = read_manifest_file(
            manifest, definition, result, artifact_sizes, existing_bytes,
            manifest_error);
        result.stats.validation_ms += elapsed_ms(validation_start);
        if (manifest_valid && cancelled_now(cancelled))
            return fail(error, "terrain collision candidate build cancelled");
    }
    if (!manifest_valid) {
        std::vector<std::uint8_t> bytes;
        if (!serialize_manifest(definition, result, artifact_sizes, bytes, error))
            return false;
        if (!publish_manifest_file(manifest, bytes, definition, result,
                                   artifact_sizes, result.stats.validation_ms,
                                   cancelled, hooks, error))
            return false;
    }
    out = std::move(result);
    error.clear();
    return true;
}

bool load_or_build_candidate_catching(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    const detail::BuildTestHooks* hooks,
    TerrainCollisionCandidate& out,
    std::string& error) {
    try {
        return load_or_build_candidate_impl(field, definition, cache_root,
                                            cancelled, hooks, out, error);
    } catch (const std::bad_alloc&) {
        out = {};
        return fail(error, "terrain collision candidate allocation failed");
    } catch (const std::filesystem::filesystem_error& exception) {
        out = {};
        return fail(error, std::string("terrain collision cache filesystem failure: ") +
                               exception.what());
    }
}

}  // namespace

namespace detail {

bool load_or_build_candidate_with_test_hooks(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    const BuildTestHooks& hooks,
    TerrainCollisionCandidate& out,
    std::string& error) {
    return load_or_build_candidate_catching(
        field, definition, cache_root, cancelled, &hooks, out, error);
}

bool validate_tile_candidate(const TileCandidate& tile,
                             const CanonicalDefinition& definition,
                             const SectorCoordinate& coordinate,
                             std::string& error) {
    if (!coordinate_in_definition(definition, coordinate))
        return fail(error, "terrain collision tile coordinate is not canonical");
    Float3 origin{};
    if (!expected_origin(definition, coordinate, origin, error)) return false;
    if (!(tile.coordinate == coordinate) ||
        !same_float3_bits(tile.origin_m, origin) ||
        tile.tile_key != tile_key_for(definition, coordinate))
        return fail(error, "terrain collision tile coordinate, origin, or key is wrong");
    if (tile.vertices.size() > kBoxCountLimit ||
        tile.indices.size() / 3u > kBoxCountLimit)
        return fail(error, "terrain collision tile exceeds Box3D signed limits");
    if (tile.indices.size() % 3u != 0u)
        return fail(error, "terrain collision tile index count is not triangular");
    if (tile.vertices.empty() != tile.indices.empty())
        return fail(error, "terrain collision empty tile has partial geometry");
    for (const Float3& vertex : tile.vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z))
            return fail(error, "terrain collision tile contains a non-finite vertex");
        if (!vertex_in_bounds(vertex, definition))
            return fail(error, "terrain collision tile vertex is outside its local bounds");
    }
    for (std::size_t offset = 0u; offset != tile.indices.size(); offset += 3u) {
        const std::uint32_t ia = tile.indices[offset + 0u];
        const std::uint32_t ib = tile.indices[offset + 1u];
        const std::uint32_t ic = tile.indices[offset + 2u];
        if (ia >= tile.vertices.size() || ib >= tile.vertices.size() ||
            ic >= tile.vertices.size())
            return fail(error, "terrain collision tile index is out of range");
        if (ia == ib || ib == ic || ia == ic)
            return fail(error, "terrain collision tile triangle repeats an index");
        const Float3& a = tile.vertices[ia];
        const Float3& b = tile.vertices[ib];
        const Float3& c = tile.vertices[ic];
        const double ux = static_cast<double>(b.x) - a.x;
        const double uy = static_cast<double>(b.y) - a.y;
        const double uz = static_cast<double>(b.z) - a.z;
        const double vx = static_cast<double>(c.x) - a.x;
        const double vy = static_cast<double>(c.y) - a.y;
        const double vz = static_cast<double>(c.z) - a.z;
        const double cx = uy * vz - uz * vy;
        const double cy = uz * vx - ux * vz;
        const double cz = ux * vy - uy * vx;
        if (!(cx * cx + cy * cy + cz * cz > 0.0))
            return fail(error, "terrain collision tile contains a zero-area triangle");
    }
    if (tile.digest != tile_digest_for(tile))
        return fail(error, "terrain collision tile digest mismatch");
    error.clear();
    return true;
}

bool convert_mesh_to_tile(const terrain_mesher::SectorMesh& mesh,
                          const CanonicalDefinition& definition,
                          const SectorCoordinate& coordinate,
                          TileCandidate& out,
                          std::string& error) {
    out = {};
    if (!coordinate_in_definition(definition, coordinate))
        return fail(error, "terrain collision mesh coordinate is not canonical");
    std::uint64_t total_triangles = 0u;
    std::uint64_t total_indices = 0u;
    for (const terrain_mesher::MaterialBucket& bucket : mesh.buckets) {
        if (bucket.positions.size() != bucket.normals.size() ||
            bucket.positions.size() % 9u != 0u)
            return fail(error, "terrain collision mesher bucket layout is invalid");
        const std::uint64_t triangles =
            static_cast<std::uint64_t>(bucket.positions.size() / 9u);
        if (!checked_add(total_triangles, triangles, total_triangles))
            return fail(error, "terrain collision mesh triangle count overflow");
    }
    if (total_triangles > kBoxCountLimit ||
        !checked_mul(total_triangles, 3u, total_indices))
        return fail(error, "terrain collision mesh exceeds Box3D signed limits");
    std::size_t reserve_indices = 0u;
    if (!to_size(total_indices, reserve_indices))
        return fail(error, "terrain collision mesh index count does not fit this platform");

    out.coordinate = coordinate;
    if (!expected_origin(definition, coordinate, out.origin_m, error)) return false;
    out.tile_key = tile_key_for(definition, coordinate);
    out.indices.reserve(reserve_indices);
    out.vertices.reserve(std::min<std::size_t>(
        reserve_indices, static_cast<std::size_t>(kBoxCountLimit)));
    std::map<std::array<std::uint32_t, 3>, std::uint32_t> welded;
    for (const terrain_mesher::MaterialBucket& bucket : mesh.buckets) {
        for (std::size_t offset = 0u; offset != bucket.positions.size();
             offset += 9u) {
            Float3 positions[3]{};
            Float3 normals[3]{};
            for (std::size_t vertex = 0u; vertex != 3u; ++vertex) {
                const std::size_t base = offset + vertex * 3u;
                positions[vertex] = {bucket.positions[base + 0u],
                                     bucket.positions[base + 1u],
                                     bucket.positions[base + 2u]};
                normals[vertex] = {bucket.normals[base + 0u],
                                   bucket.normals[base + 1u],
                                   bucket.normals[base + 2u]};
                if (!std::isfinite(positions[vertex].x) ||
                    !std::isfinite(positions[vertex].y) ||
                    !std::isfinite(positions[vertex].z) ||
                    !std::isfinite(normals[vertex].x) ||
                    !std::isfinite(normals[vertex].y) ||
                    !std::isfinite(normals[vertex].z))
                    return fail(error, "terrain collision mesher emitted non-finite data");
                if (!vertex_in_bounds(positions[vertex], definition))
                    return fail(error, "terrain collision mesher vertex is outside tile bounds");
            }
            const double ux = static_cast<double>(positions[1].x) - positions[0].x;
            const double uy = static_cast<double>(positions[1].y) - positions[0].y;
            const double uz = static_cast<double>(positions[1].z) - positions[0].z;
            const double vx = static_cast<double>(positions[2].x) - positions[0].x;
            const double vy = static_cast<double>(positions[2].y) - positions[0].y;
            const double vz = static_cast<double>(positions[2].z) - positions[0].z;
            const double cx = uy * vz - uz * vy;
            const double cy = uz * vx - ux * vz;
            const double cz = ux * vy - uy * vx;
            if (!(cx * cx + cy * cy + cz * cz > 0.0))
                return fail(error, "terrain collision mesher emitted a zero-area triangle");
            // Mirror mesh_sector_tiled's orientation authority: it compares
            // the right-hand cross product with the three emitted gradient
            // normals summed at the triangle, then swaps B/C when opposed.
            const double normal_x = static_cast<double>(normals[0].x) +
                                    normals[1].x + normals[2].x;
            const double normal_y = static_cast<double>(normals[0].y) +
                                    normals[1].y + normals[2].y;
            const double normal_z = static_cast<double>(normals[0].z) +
                                    normals[1].z + normals[2].z;
            if (cx * normal_x + cy * normal_y + cz * normal_z < 0.0)
                return fail(error, "terrain collision mesher winding opposes its normals");
            std::uint32_t triangle_indices[3]{};
            for (std::size_t vertex = 0u; vertex != 3u; ++vertex) {
                const std::array<std::uint32_t, 3> bits = {
                    float_bits(positions[vertex].x),
                    float_bits(positions[vertex].y),
                    float_bits(positions[vertex].z)};
                const auto found = welded.find(bits);
                if (found != welded.end()) {
                    triangle_indices[vertex] = found->second;
                    continue;
                }
                if (out.vertices.size() >= kBoxCountLimit)
                    return fail(error, "terrain collision mesh vertex count exceeds Box3D");
                const std::uint32_t index =
                    static_cast<std::uint32_t>(out.vertices.size());
                welded.emplace(bits, index);
                out.vertices.push_back(positions[vertex]);
                triangle_indices[vertex] = index;
            }
            if (triangle_indices[0] == triangle_indices[1] ||
                triangle_indices[1] == triangle_indices[2] ||
                triangle_indices[0] == triangle_indices[2])
                return fail(error, "terrain collision welded triangle repeats an index");
            out.indices.insert(out.indices.end(), std::begin(triangle_indices),
                               std::end(triangle_indices));
        }
    }
    out.digest = tile_digest_for(out);
    return validate_tile_candidate(out, definition, coordinate, error);
}

bool validate_tile_artifact_bytes(const std::vector<std::uint8_t>& bytes,
                                  const CanonicalDefinition& definition,
                                  const SectorCoordinate& coordinate,
                                  TileCandidate& out,
                                  std::uint64_t& artifact_bytes,
                                  std::string& error) {
    out = {};
    artifact_bytes = 0u;
    TileHeader header{};
    std::uint64_t expected_bytes = 0u;
    if (!parse_tile_header(bytes.data(), bytes.size(), definition, coordinate,
                           header, expected_bytes, error))
        return false;
    if (expected_bytes != static_cast<std::uint64_t>(bytes.size()))
        return fail(error, "MTCT exact file length does not match its counts");
    const std::uint64_t actual_file_digest = digest_with_zero_range(
        bytes.data(), bytes.size(), static_cast<std::size_t>(kTileFileDigestOffset),
        8u);
    if (header.file_digest != actual_file_digest)
        return fail(error, "MTCT whole-file digest mismatch");

    std::size_t vertex_count = 0u;
    std::size_t index_count = 0u;
    if (!to_size(header.vertex_count, vertex_count) ||
        !to_size(header.index_count, index_count))
        return fail(error, "MTCT counts do not fit this platform");
    TileCandidate candidate{};
    candidate.coordinate = header.coordinate;
    candidate.origin_m = header.origin_m;
    candidate.tile_key = header.tile_key;
    candidate.digest = header.tile_digest;
    candidate.vertices.resize(vertex_count);
    candidate.indices.resize(index_count);
    Reader reader(bytes.data() + static_cast<std::size_t>(kTileHeaderBytes),
                  bytes.size() - static_cast<std::size_t>(kTileHeaderBytes));
    for (Float3& vertex : candidate.vertices)
        if (!reader.f32(vertex.x) || !reader.f32(vertex.y) ||
            !reader.f32(vertex.z))
            return fail(error, "MTCT vertex payload is truncated");
    for (std::uint32_t& index : candidate.indices)
        if (!reader.u32(index))
            return fail(error, "MTCT index payload is truncated");
    if (reader.remaining() != 0u)
        return fail(error, "MTCT payload contains trailing bytes");
    if (!validate_tile_candidate(candidate, definition, coordinate, error))
        return false;
    artifact_bytes = static_cast<std::uint64_t>(bytes.size());
    out = std::move(candidate);
    return true;
}

bool validate_generation_manifest(const std::filesystem::path& path,
                                  const CanonicalDefinition& definition,
                                  const TerrainCollisionCandidate& candidate,
                                  std::string& error) {
    std::vector<std::uint64_t> artifact_sizes;
    if (!artifact_sizes_from_files(path, candidate, artifact_sizes, error))
        return false;
    std::vector<std::uint8_t> bytes;
    return read_manifest_file(path, definition, candidate, artifact_sizes, bytes,
                              error);
}

}  // namespace detail

bool load_or_build_candidate(
    const terrain_field::FieldRuntime& field,
    const CanonicalDefinition& definition,
    const std::filesystem::path& cache_root,
    const CancelCheck& cancelled,
    TerrainCollisionCandidate& out,
    std::string& error) {
    return load_or_build_candidate_catching(
        field, definition, cache_root, cancelled, nullptr, out, error);
}

}  // namespace matter::terrain_collision
