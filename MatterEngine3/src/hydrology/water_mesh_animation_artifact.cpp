#include "hydrology/water_mesh_animation_artifact.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

namespace hydrology {
namespace {

constexpr std::uint8_t kMagic[8] = {'M', 'H', 'Y', 'D', 'W', 'A', 'N', '2'};
constexpr std::uint32_t kVersion = 2u;
constexpr std::size_t kHeaderBytes = 32u;
constexpr std::uint64_t kMaxFileBytes = 1ull << 30u;
constexpr std::uint64_t kMaxPayloadBytes = kMaxFileBytes - kHeaderBytes;
constexpr std::uint32_t kFrameCount = 30u;
constexpr std::uint32_t kFramesPerSecond = 30u;
constexpr std::uint32_t kPhaseOffsetFrames = 15u;
constexpr std::uint64_t kFrameRecordBytes = 56u;
constexpr std::uint32_t kMaxIdentityBytes = 1024u;
constexpr float kNormalLengthEpsilon = 1e-8f;
std::atomic<std::uint64_t> g_validation_count{0u};

static_assert(sizeof(PackedWaterAnimationVertex) == 12u,
              "water animation packed vertex ABI must be 12 bytes");

bool fail(gpu_meshing::Error& error,
          gpu_meshing::ErrorCode code,
          const std::string& message) {
    error = {code, message};
    return false;
}

bool artifact_fail(gpu_meshing::Error& error, const std::string& message) {
    return fail(error, gpu_meshing::ErrorCode::ArtifactFailure, message);
}

bool finite(matter::Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite_bounds(const gpu_meshing::Aabb& bounds) noexcept {
    return finite(bounds.min_m) && finite(bounds.max_m) &&
           bounds.max_m.x >= bounds.min_m.x &&
           bounds.max_m.y >= bounds.min_m.y &&
           bounds.max_m.z >= bounds.min_m.z;
}

bool valid_lattice(
    const gpu_meshing::ParticleSamplingLattice& lattice,
    float visual_voxel_m) noexcept {
    return finite(lattice.origin_m) && std::isfinite(lattice.voxel_m) &&
           lattice.voxel_m > 0.0f && lattice.version <= 1u &&
           std::memcmp(&lattice.voxel_m, &visual_voxel_m,
                       sizeof(float)) == 0;
}

float component(matter::Float3 value, std::uint32_t axis) noexcept {
    if (axis == 0u) return value.x;
    if (axis == 1u) return value.y;
    return value.z;
}

void expand(gpu_meshing::Aabb& bounds,
            matter::Float3 point,
            bool& initialized) noexcept {
    if (!initialized) {
        bounds = {point, point};
        initialized = true;
        return;
    }
    bounds.min_m.x = std::min(bounds.min_m.x, point.x);
    bounds.min_m.y = std::min(bounds.min_m.y, point.y);
    bounds.min_m.z = std::min(bounds.min_m.z, point.z);
    bounds.max_m.x = std::max(bounds.max_m.x, point.x);
    bounds.max_m.y = std::max(bounds.max_m.y, point.y);
    bounds.max_m.z = std::max(bounds.max_m.z, point.z);
}

bool checked_add(std::uint64_t a,
                 std::uint64_t b,
                 std::uint64_t& result) noexcept {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    result = a + b;
    return true;
}

bool checked_multiply(std::uint64_t a,
                      std::uint64_t b,
                      std::uint64_t& result) noexcept {
    if (a != 0u && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    result = a * b;
    return true;
}

std::uint64_t digest_bytes(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t digest = 1469598103934665603ull;
    for (std::size_t index = 0u; index != size; ++index) {
        digest ^= bytes[index];
        digest *= 1099511628211ull;
    }
    return digest == 0u ? 1u : digest;
}

class Writer {
public:
    explicit Writer(std::uint64_t limit) : limit_(limit) {}

    bool u8(std::uint8_t value) {
        if (!grow(1u)) return false;
        bytes.push_back(value);
        return true;
    }
    bool u32(std::uint32_t value) {
        for (unsigned shift = 0u; shift != 32u; shift += 8u)
            if (!u8(static_cast<std::uint8_t>(value >> shift))) return false;
        return true;
    }
    bool u64(std::uint64_t value) {
        for (unsigned shift = 0u; shift != 64u; shift += 8u)
            if (!u8(static_cast<std::uint8_t>(value >> shift))) return false;
        return true;
    }
    bool floating(float value) {
        std::uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32(bits);
    }
    bool raw(const std::uint8_t* data, std::size_t size) {
        if (!grow(size)) return false;
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }
    bool string(const std::string& value) {
        return value.size() <= kMaxIdentityBytes &&
               u32(static_cast<std::uint32_t>(value.size())) &&
               raw(reinterpret_cast<const std::uint8_t*>(value.data()),
                   value.size());
    }

    std::vector<std::uint8_t> bytes;

private:
    bool grow(std::uint64_t amount) const noexcept {
        return amount <= limit_ && bytes.size() <= limit_ - amount;
    }
    std::uint64_t limit_ = 0u;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : current(data), remaining(size) {}

    bool u8(std::uint8_t& value) {
        if (remaining < 1u) return false;
        value = *current++;
        --remaining;
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0u;
        for (unsigned shift = 0u; shift != 32u; shift += 8u) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0u;
        for (unsigned shift = 0u; shift != 64u; shift += 8u) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool floating(float& value) {
        std::uint32_t bits = 0u;
        if (!u32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t size = 0u;
        if (!u32(size) || size > kMaxIdentityBytes || size > remaining)
            return false;
        value.assign(reinterpret_cast<const char*>(current), size);
        current += size;
        remaining -= size;
        return true;
    }
    bool raw(std::vector<std::uint8_t>& value, std::size_t size) {
        if (size > remaining) return false;
        value.assign(current, current + size);
        current += size;
        remaining -= size;
        return true;
    }

    const std::uint8_t* current = nullptr;
    std::size_t remaining = 0u;
};

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

float sign_not_zero(float value) noexcept {
    return value < 0.0f ? -1.0f : 1.0f;
}

std::int16_t pack_snorm16(float value) noexcept {
    const float clamped = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
}

float unpack_snorm16(std::uint16_t bits) noexcept {
    const auto value = static_cast<std::int16_t>(bits);
    return std::max(-1.0f,
                    static_cast<float>(value) / 32767.0f);
}

bool normalize(matter::Float3 input, matter::Float3& output) noexcept {
    const double length_squared =
        static_cast<double>(input.x) * input.x +
        static_cast<double>(input.y) * input.y +
        static_cast<double>(input.z) * input.z;
    if (!std::isfinite(length_squared) ||
        length_squared <= kNormalLengthEpsilon * kNormalLengthEpsilon)
        return false;
    const float inverse =
        static_cast<float>(1.0 / std::sqrt(length_squared));
    output = {input.x * inverse, input.y * inverse, input.z * inverse};
    return finite(output);
}

bool encode_octahedral(matter::Float3 normal,
                       std::int16_t& x,
                       std::int16_t& y) noexcept {
    matter::Float3 unit{};
    if (!normalize(normal, unit)) return false;
    const float inverse_l1 =
        1.0f / (std::fabs(unit.x) + std::fabs(unit.y) +
                std::fabs(unit.z));
    float ox = unit.x * inverse_l1;
    float oy = unit.y * inverse_l1;
    if (unit.z < 0.0f) {
        const float old_x = ox;
        ox = (1.0f - std::fabs(oy)) * sign_not_zero(old_x);
        oy = (1.0f - std::fabs(old_x)) * sign_not_zero(oy);
    }
    x = pack_snorm16(ox);
    y = pack_snorm16(oy);
    return true;
}

matter::Float3 decode_octahedral(std::int16_t x,
                                 std::int16_t y) noexcept {
    matter::Float3 normal{
        unpack_snorm16(static_cast<std::uint16_t>(x)),
        unpack_snorm16(static_cast<std::uint16_t>(y)),
        0.0f,
    };
    normal.z = 1.0f - std::fabs(normal.x) - std::fabs(normal.y);
    if (normal.z < 0.0f) {
        const float old_x = normal.x;
        normal.x =
            (1.0f - std::fabs(normal.y)) * sign_not_zero(old_x);
        normal.y =
            (1.0f - std::fabs(old_x)) * sign_not_zero(normal.y);
    }
    matter::Float3 unit{};
    if (!normalize(normal, unit)) return {0.0f, 1.0f, 0.0f};
    return unit;
}

std::uint16_t quantize_position(float value,
                                float minimum,
                                float maximum) noexcept {
    const float extent = maximum - minimum;
    if (extent <= 0.0f) return 0u;
    const double normalized = std::max(
        0.0, std::min(1.0,
                      (static_cast<double>(value) - minimum) / extent));
    return static_cast<std::uint16_t>(
        std::llround(normalized * 65535.0));
}

float dequantize_position(std::uint16_t value,
                          float minimum,
                          float maximum) noexcept {
    return minimum + (maximum - minimum) *
        (static_cast<float>(value) / 65535.0f);
}

PackedWaterAnimationVertex encode_vertex(
    matter::Float3 position,
    matter::Float3 normal,
    const gpu_meshing::Aabb& bounds) noexcept {
    const std::uint16_t x = quantize_position(
        position.x, bounds.min_m.x, bounds.max_m.x);
    const std::uint16_t y = quantize_position(
        position.y, bounds.min_m.y, bounds.max_m.y);
    const std::uint16_t z = quantize_position(
        position.z, bounds.min_m.z, bounds.max_m.z);
    std::int16_t normal_x = 0;
    std::int16_t normal_y = 0;
    (void)encode_octahedral(normal, normal_x, normal_y);
    return {
        static_cast<std::uint32_t>(x) |
            (static_cast<std::uint32_t>(y) << 16u),
        static_cast<std::uint32_t>(z) |
            (static_cast<std::uint32_t>(
                 static_cast<std::uint16_t>(normal_x)) << 16u),
        static_cast<std::uint32_t>(
            static_cast<std::uint16_t>(normal_y)),
    };
}

void decode_vertex(const PackedWaterAnimationVertex& packed,
                   const gpu_meshing::Aabb& bounds,
                   matter::Float3& position,
                   matter::Float3& normal) noexcept {
    const auto x = static_cast<std::uint16_t>(
        packed.position_xy_unorm16 & 0xffffu);
    const auto y = static_cast<std::uint16_t>(
        packed.position_xy_unorm16 >> 16u);
    const auto z = static_cast<std::uint16_t>(
        packed.position_z_unorm16_normal_x_snorm16 & 0xffffu);
    const auto nx = static_cast<std::int16_t>(
        packed.position_z_unorm16_normal_x_snorm16 >> 16u);
    const auto ny = static_cast<std::int16_t>(
        packed.normal_y_snorm16_reserved & 0xffffu);
    position = {
        dequantize_position(x, bounds.min_m.x, bounds.max_m.x),
        dequantize_position(y, bounds.min_m.y, bounds.max_m.y),
        dequantize_position(z, bounds.min_m.z, bounds.max_m.z),
    };
    normal = decode_octahedral(nx, ny);
}

PackedWaterAnimationVertex read_packed_vertex(
    const std::uint8_t* bytes) noexcept {
    return {read_u32(bytes), read_u32(bytes + 4u), read_u32(bytes + 8u)};
}

bool write_packed_vertex(Writer& writer,
                         const PackedWaterAnimationVertex& vertex) {
    return writer.u32(vertex.position_xy_unorm16) &&
           writer.u32(vertex.position_z_unorm16_normal_x_snorm16) &&
           writer.u32(vertex.normal_y_snorm16_reserved);
}

float triangle_area_squared(matter::Float3 a,
                            matter::Float3 b,
                            matter::Float3 c) noexcept {
    const matter::Float3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    const matter::Float3 ac{c.x - a.x, c.y - a.y, c.z - a.z};
    const matter::Float3 cross{
        ab.y * ac.z - ab.z * ac.y,
        ab.z * ac.x - ab.x * ac.z,
        ab.x * ac.y - ab.y * ac.x,
    };
    return cross.x * cross.x + cross.y * cross.y + cross.z * cross.z;
}

bool validate_source_mesh(const gpu_meshing::MeshResult& mesh,
                          std::uint32_t expected_material) {
    if (mesh.material != expected_material || mesh.positions.empty() ||
        mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u || mesh.indices.empty() ||
        mesh.indices.size() % 3u != 0u ||
        mesh.content_digest != gpu_meshing::mesh_content_digest(mesh))
        return false;
    for (float value : mesh.positions)
        if (!std::isfinite(value)) return false;
    for (float value : mesh.normals)
        if (!std::isfinite(value)) return false;
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    for (std::uint32_t index : mesh.indices)
        if (index >= vertex_count) return false;
    return true;
}

bool frame_range(const WaterMeshAnimationFrameRecord& frame,
                 std::uint64_t payload_size,
                 std::uint64_t& vertex_bytes,
                 std::uint64_t& index_bytes,
                 std::uint64_t& end) noexcept {
    if (!checked_multiply(frame.vertex_count,
                          sizeof(PackedWaterAnimationVertex),
                          vertex_bytes) ||
        !checked_multiply(frame.index_count, sizeof(std::uint32_t),
                          index_bytes) ||
        frame.vertex_payload_offset > payload_size ||
        vertex_bytes > payload_size - frame.vertex_payload_offset)
        return false;
    const std::uint64_t vertex_end =
        frame.vertex_payload_offset + vertex_bytes;
    if (frame.index_payload_offset != vertex_end ||
        frame.index_payload_offset > payload_size ||
        index_bytes > payload_size - frame.index_payload_offset)
        return false;
    end = frame.index_payload_offset + index_bytes;
    return true;
}

bool validate_artifact(const WaterMeshAnimationArtifact& artifact,
                       gpu_meshing::Error& error) {
    g_validation_count.fetch_add(1u, std::memory_order_relaxed);
    if (artifact.identity.empty() ||
        artifact.identity.size() > kMaxIdentityBytes ||
        artifact.semantic_key == 0u ||
        artifact.source_primary_payload_digest == 0u ||
        artifact.frames_per_second != kFramesPerSecond ||
        artifact.phase_offset_frames != kPhaseOffsetFrames ||
        artifact.duration_seconds != 1.0f ||
        !std::isfinite(artifact.visual_voxel_m) ||
        artifact.visual_voxel_m <= 0.0f ||
        !valid_lattice(artifact.lattice, artifact.visual_voxel_m) ||
        artifact.frames.size() != kFrameCount ||
        !finite_bounds(artifact.quantization_bounds_m)) {
        return artifact_fail(error,
                             "water animation artifact metadata is invalid");
    }
    if (artifact.frame_payload.size() > kMaxPayloadBytes)
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water animation artifact exceeds one GiB");
    for (std::uint32_t axis = 0u; axis != 3u; ++axis) {
        const float extent =
            component(artifact.quantization_bounds_m.max_m, axis) -
            component(artifact.quantization_bounds_m.min_m, axis);
        if (extent / (2.0f * 65535.0f) >
            artifact.visual_voxel_m / 16.0f) {
            return artifact_fail(
                error, "water animation position quantization exceeds tolerance");
        }
    }

    std::uint64_t expected_offset = 0u;
    for (const WaterMeshAnimationFrameRecord& frame : artifact.frames) {
        if (frame.vertex_count == 0u || frame.index_count == 0u ||
            frame.index_count % 3u != 0u ||
            !finite_bounds(frame.bounds_m) ||
            frame.bounds_m.min_m.x < artifact.quantization_bounds_m.min_m.x ||
            frame.bounds_m.min_m.y < artifact.quantization_bounds_m.min_m.y ||
            frame.bounds_m.min_m.z < artifact.quantization_bounds_m.min_m.z ||
            frame.bounds_m.max_m.x > artifact.quantization_bounds_m.max_m.x ||
            frame.bounds_m.max_m.y > artifact.quantization_bounds_m.max_m.y ||
            frame.bounds_m.max_m.z > artifact.quantization_bounds_m.max_m.z) {
            return artifact_fail(error,
                                 "water animation frame directory is invalid");
        }
        std::uint64_t vertex_bytes = 0u;
        std::uint64_t index_bytes = 0u;
        std::uint64_t end = 0u;
        if (frame.vertex_payload_offset != expected_offset ||
            !frame_range(frame, artifact.frame_payload.size(), vertex_bytes,
                         index_bytes, end)) {
            return artifact_fail(error,
                                 "water animation frame offsets are invalid");
        }
        const std::uint8_t* begin = artifact.frame_payload.data() +
            static_cast<std::size_t>(frame.vertex_payload_offset);
        if (digest_bytes(begin, static_cast<std::size_t>(end - expected_offset)) !=
            frame.content_digest) {
            return artifact_fail(error,
                                 "water animation frame digest is invalid");
        }
        std::vector<matter::Float3> positions(frame.vertex_count);
        for (std::uint32_t vertex = 0u; vertex != frame.vertex_count;
             ++vertex) {
            const std::uint8_t* source = begin +
                static_cast<std::size_t>(vertex) *
                    sizeof(PackedWaterAnimationVertex);
            const PackedWaterAnimationVertex packed =
                read_packed_vertex(source);
            if ((packed.normal_y_snorm16_reserved & 0xffff0000u) != 0u)
                return artifact_fail(
                    error, "water animation packed vertex reserved bits are nonzero");
            matter::Float3 normal{};
            decode_vertex(packed, artifact.quantization_bounds_m,
                          positions[vertex], normal);
            const float length_squared = normal.x * normal.x +
                normal.y * normal.y + normal.z * normal.z;
            if (!finite(positions[vertex]) || !finite(normal) ||
                std::fabs(length_squared - 1.0f) > 1e-4f) {
                return artifact_fail(
                    error, "water animation packed vertex is invalid");
            }
        }
        const std::uint8_t* index_data = artifact.frame_payload.data() +
            static_cast<std::size_t>(frame.index_payload_offset);
        for (std::uint32_t index = 0u; index != frame.index_count; ++index) {
            if (read_u32(index_data + static_cast<std::size_t>(index) * 4u) >=
                frame.vertex_count) {
                return artifact_fail(error,
                                     "water animation index is out of range");
            }
        }
        for (std::uint32_t index = 0u; index != frame.index_count;
             index += 3u) {
            const std::uint32_t a = read_u32(
                index_data + static_cast<std::size_t>(index) * 4u);
            const std::uint32_t b = read_u32(
                index_data + static_cast<std::size_t>(index + 1u) * 4u);
            const std::uint32_t c = read_u32(
                index_data + static_cast<std::size_t>(index + 2u) * 4u);
            if (triangle_area_squared(positions[a], positions[b], positions[c]) <=
                1e-20f) {
                return artifact_fail(
                    error, "water animation quantization introduced a zero-area triangle");
            }
        }
        expected_offset = end;
    }
    if (expected_offset != artifact.frame_payload.size())
        return artifact_fail(error,
                             "water animation payload has trailing bytes");
    error = {};
    return true;
}

bool write_bounds(Writer& writer, const gpu_meshing::Aabb& bounds) {
    return writer.floating(bounds.min_m.x) &&
           writer.floating(bounds.min_m.y) &&
           writer.floating(bounds.min_m.z) &&
           writer.floating(bounds.max_m.x) &&
           writer.floating(bounds.max_m.y) &&
           writer.floating(bounds.max_m.z);
}

bool read_bounds(Reader& reader, gpu_meshing::Aabb& bounds) {
    return reader.floating(bounds.min_m.x) &&
           reader.floating(bounds.min_m.y) &&
           reader.floating(bounds.min_m.z) &&
           reader.floating(bounds.max_m.x) &&
           reader.floating(bounds.max_m.y) &&
           reader.floating(bounds.max_m.z);
}

bool read_file(const std::filesystem::path& path,
               std::vector<std::uint8_t>& bytes,
               gpu_meshing::Error& error) {
    std::error_code filesystem_error;
    const std::uintmax_t size =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error)
        return artifact_fail(error,
                             "could not stat water animation artifact");
    if (size > kMaxFileBytes)
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water animation artifact exceeds one GiB");
    if (size < kHeaderBytes)
        return artifact_fail(error,
                             "water animation artifact is truncated");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return artifact_fail(error,
                             "could not open water animation artifact");
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size())
        return artifact_fail(error,
                             "could not read water animation artifact");
    return true;
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes,
                gpu_meshing::Error& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return artifact_fail(error,
                             "could not create water animation temporary");
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream)
        return artifact_fail(error,
                             "could not write water animation temporary");
    return true;
}

}  // namespace

std::uint64_t water_mesh_animation_validation_count() noexcept {
    return g_validation_count.load(std::memory_order_relaxed);
}

bool operator==(const WaterMeshAnimationFrameRecord& a,
                const WaterMeshAnimationFrameRecord& b) noexcept {
    return a.vertex_payload_offset == b.vertex_payload_offset &&
           a.index_payload_offset == b.index_payload_offset &&
           a.vertex_count == b.vertex_count &&
           a.index_count == b.index_count &&
           a.bounds_m.min_m.x == b.bounds_m.min_m.x &&
           a.bounds_m.min_m.y == b.bounds_m.min_m.y &&
           a.bounds_m.min_m.z == b.bounds_m.min_m.z &&
           a.bounds_m.max_m.x == b.bounds_m.max_m.x &&
           a.bounds_m.max_m.y == b.bounds_m.max_m.y &&
           a.bounds_m.max_m.z == b.bounds_m.max_m.z &&
           a.content_digest == b.content_digest;
}

bool pack_water_mesh_animation_artifact(
    const WaterMeshAnimationArtifactMetadata& metadata,
    const WaterMeshAnimation& animation,
    WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    if (metadata.identity.empty() ||
        metadata.identity.size() > kMaxIdentityBytes ||
        metadata.semantic_key == 0u ||
        metadata.source_primary_payload_digest == 0u ||
        !std::isfinite(metadata.visual_voxel_m) ||
        metadata.visual_voxel_m <= 0.0f ||
        !valid_lattice(metadata.lattice, metadata.visual_voxel_m) ||
        animation.frames_per_second != kFramesPerSecond ||
        animation.phase_offset_frames != kPhaseOffsetFrames ||
        animation.duration_seconds != 1.0f ||
        animation.frames.size() != kFrameCount) {
        return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                    "water animation source metadata is invalid");
    }
    const std::uint32_t material = animation.frames.front().material;
    gpu_meshing::Aabb shared_bounds{};
    bool shared_initialized = false;
    std::vector<gpu_meshing::Aabb> frame_bounds(kFrameCount);
    std::uint64_t total_frame_bytes = 0u;
    for (std::uint32_t frame_index = 0u; frame_index != kFrameCount;
         ++frame_index) {
        const auto& mesh = animation.frames[frame_index];
        if (!validate_source_mesh(mesh, material)) {
            return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                        "water animation source frame " +
                            std::to_string(frame_index) + " is invalid");
        }
        bool frame_initialized = false;
        for (std::size_t vertex = 0u;
             vertex != mesh.positions.size() / 3u; ++vertex) {
            const matter::Float3 position{
                mesh.positions[vertex * 3u + 0u],
                mesh.positions[vertex * 3u + 1u],
                mesh.positions[vertex * 3u + 2u],
            };
            matter::Float3 unit{};
            const matter::Float3 normal{
                mesh.normals[vertex * 3u + 0u],
                mesh.normals[vertex * 3u + 1u],
                mesh.normals[vertex * 3u + 2u],
            };
            if (!finite(position) || !normalize(normal, unit)) {
                return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                            "water animation source frame " +
                                std::to_string(frame_index) +
                                " contains invalid vertex data");
            }
            expand(frame_bounds[frame_index], position, frame_initialized);
            expand(shared_bounds, position, shared_initialized);
        }
        std::uint64_t vertex_bytes = 0u;
        std::uint64_t index_bytes = 0u;
        std::uint64_t frame_bytes = 0u;
        if (!checked_multiply(mesh.positions.size() / 3u,
                              sizeof(PackedWaterAnimationVertex),
                              vertex_bytes) ||
            !checked_multiply(mesh.indices.size(), sizeof(std::uint32_t),
                              index_bytes) ||
            !checked_add(vertex_bytes, index_bytes, frame_bytes) ||
            !checked_add(total_frame_bytes, frame_bytes,
                         total_frame_bytes) ||
            total_frame_bytes > kMaxPayloadBytes) {
            return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                        "water animation packed payload exceeds one GiB");
        }
    }
    for (std::uint32_t axis = 0u; axis != 3u; ++axis) {
        const float extent = component(shared_bounds.max_m, axis) -
            component(shared_bounds.min_m, axis);
        if (extent / (2.0f * 65535.0f) >
            metadata.visual_voxel_m / 16.0f) {
            return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                        "water animation bounds exceed quantization tolerance");
        }
    }

    WaterMeshAnimationArtifact candidate{};
    candidate.identity = metadata.identity;
    candidate.semantic_key = metadata.semantic_key;
    candidate.source_primary_payload_digest =
        metadata.source_primary_payload_digest;
    candidate.source_secondary_payload_digest =
        metadata.source_secondary_payload_digest;
    candidate.frames_per_second = animation.frames_per_second;
    candidate.phase_offset_frames = animation.phase_offset_frames;
    candidate.duration_seconds = animation.duration_seconds;
    candidate.visual_voxel_m = metadata.visual_voxel_m;
    candidate.lattice = metadata.lattice;
    candidate.material = material;
    candidate.quantization_bounds_m = shared_bounds;
    candidate.frames.reserve(kFrameCount);
    Writer payload(kMaxPayloadBytes);
    payload.bytes.reserve(static_cast<std::size_t>(total_frame_bytes));
    for (std::uint32_t frame_index = 0u; frame_index != kFrameCount;
         ++frame_index) {
        const auto& mesh = animation.frames[frame_index];
        WaterMeshAnimationFrameRecord frame{};
        frame.vertex_payload_offset = payload.bytes.size();
        frame.vertex_count =
            static_cast<std::uint32_t>(mesh.positions.size() / 3u);
        frame.index_count = 0u;
        frame.bounds_m = frame_bounds[frame_index];
        std::vector<matter::Float3> decoded;
        decoded.reserve(frame.vertex_count);
        for (std::uint32_t vertex = 0u; vertex != frame.vertex_count;
             ++vertex) {
            const matter::Float3 position{
                mesh.positions[vertex * 3u + 0u],
                mesh.positions[vertex * 3u + 1u],
                mesh.positions[vertex * 3u + 2u],
            };
            const matter::Float3 normal{
                mesh.normals[vertex * 3u + 0u],
                mesh.normals[vertex * 3u + 1u],
                mesh.normals[vertex * 3u + 2u],
            };
            const PackedWaterAnimationVertex packed =
                encode_vertex(position, normal, shared_bounds);
            matter::Float3 decoded_position{};
            matter::Float3 decoded_normal{};
            decode_vertex(packed, shared_bounds, decoded_position,
                          decoded_normal);
            for (std::uint32_t axis = 0u; axis != 3u; ++axis) {
                if (std::fabs(component(position, axis) -
                              component(decoded_position, axis)) >
                    metadata.visual_voxel_m / 16.0f) {
                    return fail(
                        error, gpu_meshing::ErrorCode::LimitExceeded,
                        "water animation position quantization exceeds tolerance");
                }
            }
            matter::Float3 unit_normal{};
            if (!normalize(normal, unit_normal))
                return fail(error, gpu_meshing::ErrorCode::InvalidInput,
                            "water animation normal is invalid");
            const float dot = unit_normal.x * decoded_normal.x +
                unit_normal.y * decoded_normal.y +
                unit_normal.z * decoded_normal.z;
            if (!std::isfinite(dot) ||
                dot < std::cos(3.14159265358979323846f / 180.0f)) {
                return fail(error, gpu_meshing::ErrorCode::ArtifactFailure,
                            "water animation normal quantization exceeds one degree");
            }
            if (!write_packed_vertex(payload, packed))
                return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                            "water animation packed payload exceeds one GiB");
            decoded.push_back(decoded_position);
        }
        frame.index_payload_offset = payload.bytes.size();
        for (std::size_t index = 0u; index != mesh.indices.size(); index += 3u) {
            if (triangle_area_squared(decoded[mesh.indices[index]],
                                      decoded[mesh.indices[index + 1u]],
                                      decoded[mesh.indices[index + 2u]]) <=
                1e-20f) {
                continue;
            }
            if (!payload.u32(mesh.indices[index]) ||
                !payload.u32(mesh.indices[index + 1u]) ||
                !payload.u32(mesh.indices[index + 2u])) {
                return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                            "water animation packed payload exceeds one GiB");
            }
            frame.index_count += 3u;
        }
        if (frame.index_count == 0u) {
            return fail(
                error, gpu_meshing::ErrorCode::ArtifactFailure,
                "water animation quantization removed every triangle in a frame");
        }
        const std::size_t begin =
            static_cast<std::size_t>(frame.vertex_payload_offset);
        frame.content_digest = digest_bytes(
            payload.bytes.data() + begin, payload.bytes.size() - begin);
        candidate.frames.push_back(frame);
    }
    candidate.frame_payload = std::move(payload.bytes);
    if (!validate_artifact(candidate, error)) return false;
    artifact = std::move(candidate);
    return true;
}

bool serialize_water_mesh_animation_artifact(
    const WaterMeshAnimationArtifact& artifact,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error) {
    bytes.clear();
    error = {};
    if (!validate_artifact(artifact, error)) return false;
    Writer payload(kMaxPayloadBytes);
    std::uint64_t directory_bytes = 0u;
    if (!checked_multiply(artifact.frames.size(), kFrameRecordBytes,
                          directory_bytes)) {
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water animation directory size overflowed");
    }
    bool ok = payload.string(artifact.identity) &&
        payload.u64(artifact.semantic_key) &&
        payload.u64(artifact.source_primary_payload_digest) &&
        payload.u64(artifact.source_secondary_payload_digest) &&
        payload.u32(artifact.frames_per_second) &&
        payload.u32(static_cast<std::uint32_t>(artifact.frames.size())) &&
        payload.u32(artifact.phase_offset_frames) &&
        payload.u32(artifact.material) &&
        payload.floating(artifact.duration_seconds) &&
        payload.floating(artifact.visual_voxel_m) &&
        payload.floating(artifact.lattice.origin_m.x) &&
        payload.floating(artifact.lattice.origin_m.y) &&
        payload.floating(artifact.lattice.origin_m.z) &&
        payload.floating(artifact.lattice.voxel_m) &&
        payload.u32(artifact.lattice.version) &&
        write_bounds(payload, artifact.quantization_bounds_m) &&
        payload.u64(directory_bytes);
    for (const WaterMeshAnimationFrameRecord& frame : artifact.frames) {
        ok = ok && payload.u64(frame.vertex_payload_offset) &&
            payload.u64(frame.index_payload_offset) &&
            payload.u32(frame.vertex_count) && payload.u32(frame.index_count) &&
            write_bounds(payload, frame.bounds_m) &&
            payload.u64(frame.content_digest);
    }
    ok = ok && payload.u64(artifact.frame_payload.size()) &&
        payload.raw(artifact.frame_payload.data(),
                    artifact.frame_payload.size());
    if (!ok)
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water animation artifact exceeds one GiB");
    const std::uint64_t digest =
        digest_bytes(payload.bytes.data(), payload.bytes.size());
    Writer file(kMaxFileBytes);
    if (!file.raw(kMagic, sizeof(kMagic)) || !file.u32(kVersion) ||
        !file.u32(0u) || !file.u64(payload.bytes.size()) ||
        !file.u64(digest) ||
        !file.raw(payload.bytes.data(), payload.bytes.size())) {
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water animation artifact exceeds one GiB");
    }
    bytes = std::move(file.bytes);
    return true;
}

bool deserialize_water_mesh_animation_artifact(
    const std::vector<std::uint8_t>& bytes,
    WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    if (bytes.size() < kHeaderBytes)
        return artifact_fail(error,
                             "water animation artifact is truncated");
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return artifact_fail(error,
                             "water animation artifact magic is invalid");
    Reader header(bytes.data() + sizeof(kMagic),
                  bytes.size() - sizeof(kMagic));
    std::uint32_t version = 0u;
    std::uint32_t reserved = 0u;
    std::uint64_t payload_size = 0u;
    std::uint64_t expected_digest = 0u;
    if (!header.u32(version) || !header.u32(reserved) ||
        !header.u64(payload_size) || !header.u64(expected_digest))
        return artifact_fail(error,
                             "water animation artifact header is truncated");
    if (payload_size > kMaxPayloadBytes)
        return fail(error, gpu_meshing::ErrorCode::LimitExceeded,
                    "water animation artifact exceeds one GiB");
    if (version != kVersion || reserved != 0u ||
        payload_size != bytes.size() - kHeaderBytes)
        return artifact_fail(error,
                             "water animation artifact header is invalid");
    const std::uint8_t* payload_bytes = bytes.data() + kHeaderBytes;
    if (digest_bytes(payload_bytes, static_cast<std::size_t>(payload_size)) !=
        expected_digest) {
        return artifact_fail(error,
                             "water animation artifact payload digest is invalid");
    }
    Reader reader(payload_bytes, static_cast<std::size_t>(payload_size));
    WaterMeshAnimationArtifact candidate{};
    std::uint32_t frame_count = 0u;
    std::uint64_t directory_bytes = 0u;
    if (!reader.string(candidate.identity) ||
        !reader.u64(candidate.semantic_key) ||
        !reader.u64(candidate.source_primary_payload_digest) ||
        !reader.u64(candidate.source_secondary_payload_digest) ||
        !reader.u32(candidate.frames_per_second) ||
        !reader.u32(frame_count) ||
        !reader.u32(candidate.phase_offset_frames) ||
        !reader.u32(candidate.material) ||
        !reader.floating(candidate.duration_seconds) ||
        !reader.floating(candidate.visual_voxel_m) ||
        !reader.floating(candidate.lattice.origin_m.x) ||
        !reader.floating(candidate.lattice.origin_m.y) ||
        !reader.floating(candidate.lattice.origin_m.z) ||
        !reader.floating(candidate.lattice.voxel_m) ||
        !reader.u32(candidate.lattice.version) ||
        !read_bounds(reader, candidate.quantization_bounds_m) ||
        !reader.u64(directory_bytes) || frame_count != kFrameCount ||
        directory_bytes != kFrameRecordBytes * frame_count ||
        directory_bytes > reader.remaining) {
        return artifact_fail(error,
                             "water animation artifact metadata is invalid");
    }
    candidate.frames.resize(frame_count);
    for (WaterMeshAnimationFrameRecord& frame : candidate.frames) {
        if (!reader.u64(frame.vertex_payload_offset) ||
            !reader.u64(frame.index_payload_offset) ||
            !reader.u32(frame.vertex_count) ||
            !reader.u32(frame.index_count) ||
            !read_bounds(reader, frame.bounds_m) ||
            !reader.u64(frame.content_digest)) {
            return artifact_fail(error,
                                 "water animation frame directory is truncated");
        }
    }
    std::uint64_t frame_payload_size = 0u;
    if (!reader.u64(frame_payload_size) ||
        frame_payload_size > kMaxPayloadBytes ||
        frame_payload_size != reader.remaining ||
        !reader.raw(candidate.frame_payload,
                    static_cast<std::size_t>(frame_payload_size)) ||
        reader.remaining != 0u) {
        return artifact_fail(error,
                             "water animation frame payload is invalid");
    }
    candidate.payload_digest = expected_digest;
    if (!validate_artifact(candidate, error)) return false;
    artifact = std::move(candidate);
    return true;
}

bool water_mesh_animation_frame_span(
    const WaterMeshAnimationArtifact& artifact,
    std::uint32_t frame_index,
    WaterMeshAnimationFrameSpan& span,
    gpu_meshing::Error& error) {
    span = {};
    if (!validate_artifact(artifact, error)) return false;
    if (frame_index >= artifact.frames.size())
        return artifact_fail(error,
                             "water animation frame index is out of range");
    const WaterMeshAnimationFrameRecord& frame = artifact.frames[frame_index];
    std::uint64_t vertex_bytes = 0u;
    std::uint64_t index_bytes = 0u;
    std::uint64_t end = 0u;
    if (!frame_range(frame, artifact.frame_payload.size(), vertex_bytes,
                     index_bytes, end)) {
        return artifact_fail(error,
                             "water animation frame offsets are invalid");
    }
    span.vertex_data = artifact.frame_payload.data() +
        static_cast<std::size_t>(frame.vertex_payload_offset);
    span.index_data = artifact.frame_payload.data() +
        static_cast<std::size_t>(frame.index_payload_offset);
    span.vertex_bytes = static_cast<std::size_t>(vertex_bytes);
    span.index_bytes = static_cast<std::size_t>(index_bytes);
    span.vertex_count = frame.vertex_count;
    span.index_count = frame.index_count;
    error = {};
    return true;
}

bool decode_water_mesh_animation_frame(
    const WaterMeshAnimationArtifact& artifact,
    std::uint32_t frame_index,
    gpu_meshing::MeshResult& mesh,
    gpu_meshing::Error& error) {
    mesh = {};
    WaterMeshAnimationFrameSpan span{};
    if (!water_mesh_animation_frame_span(artifact, frame_index, span, error))
        return false;
    gpu_meshing::MeshResult candidate{};
    candidate.positions.reserve(static_cast<std::size_t>(span.vertex_count) * 3u);
    candidate.normals.reserve(static_cast<std::size_t>(span.vertex_count) * 3u);
    candidate.indices.reserve(span.index_count);
    candidate.material = artifact.material;
    for (std::uint32_t vertex = 0u; vertex != span.vertex_count; ++vertex) {
        const PackedWaterAnimationVertex packed = read_packed_vertex(
            span.vertex_data + static_cast<std::size_t>(vertex) *
                sizeof(PackedWaterAnimationVertex));
        matter::Float3 position{};
        matter::Float3 normal{};
        decode_vertex(packed, artifact.quantization_bounds_m,
                      position, normal);
        candidate.positions.insert(candidate.positions.end(),
                                   {position.x, position.y, position.z});
        candidate.normals.insert(candidate.normals.end(),
                                 {normal.x, normal.y, normal.z});
    }
    for (std::uint32_t index = 0u; index != span.index_count; ++index) {
        candidate.indices.push_back(read_u32(
            span.index_data + static_cast<std::size_t>(index) * 4u));
    }
    candidate.content_digest = gpu_meshing::mesh_content_digest(candidate);
    mesh = std::move(candidate);
    error = {};
    return true;
}

bool save_water_mesh_animation_artifact_immutable(
    const std::filesystem::path& path,
    const WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_water_mesh_animation_artifact(artifact, bytes, error))
        return false;
    std::error_code filesystem_error;
    if (std::filesystem::exists(path, filesystem_error)) {
        std::vector<std::uint8_t> installed;
        if (!read_file(path, installed, error)) return false;
        if (installed == bytes) return true;
        return artifact_fail(
            error, "immutable water animation artifact already has different content");
    }
    if (filesystem_error)
        return artifact_fail(error,
                             "could not inspect water animation artifact path");
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystem_error);
        if (filesystem_error)
            return artifact_fail(
                error, "could not create water animation artifact directory");
    }
    static std::atomic<std::uint64_t> serial{0u};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++serial);
    if (!write_file(temporary, bytes, error)) return false;
    WaterMeshAnimationArtifact reopened{};
    if (!load_water_mesh_animation_artifact(temporary, reopened, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    std::error_code link_error;
    std::filesystem::create_hard_link(temporary, path, link_error);
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    if (!link_error) return true;

    filesystem_error.clear();
    if (std::filesystem::exists(path, filesystem_error)) {
        std::vector<std::uint8_t> installed;
        if (!read_file(path, installed, error)) return false;
        if (installed == bytes) return true;
        return artifact_fail(
            error, "immutable water animation artifact already has different content");
    }
    return artifact_fail(error,
                         "could not install immutable water animation artifact");
}

bool load_water_mesh_animation_artifact(
    const std::filesystem::path& path,
    WaterMeshAnimationArtifact& artifact,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) return false;
    return deserialize_water_mesh_animation_artifact(bytes, artifact, error);
}

}  // namespace hydrology
