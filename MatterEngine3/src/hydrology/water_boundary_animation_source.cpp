#include "hydrology/water_boundary_animation_source.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <string>

namespace hydrology {
namespace {

constexpr std::uint8_t kMagic[8] = {'M', 'H', 'Y', 'D', 'W', 'B', 'S', '1'};
constexpr std::uint32_t kVersion = 1u;
constexpr std::uint32_t kHeaderBytes = 32u;
constexpr std::uint64_t kMaxFileBytes = 1ull << 30u;
constexpr std::uint64_t kMaxBodyBytes = kMaxFileBytes - kHeaderBytes;
constexpr std::uint32_t kFrameCount = 30u;
constexpr std::uint32_t kFramesPerSecond = 30u;
constexpr std::uint32_t kPhaseOffsetFrames = 15u;
constexpr std::uint64_t kFrameRecordBytes = 24u;
constexpr std::uint64_t kPositionBytes = 6u;
constexpr std::uint32_t kMaxSectionIdBytes = 1024u;

bool fail(gpu_meshing::Error& error,
          gpu_meshing::ErrorCode code,
          const std::string& message) {
    error = {code, message};
    return false;
}

bool artifact_fail(gpu_meshing::Error& error, const std::string& message) {
    return fail(error, gpu_meshing::ErrorCode::ArtifactFailure, message);
}

bool limit_fail(gpu_meshing::Error& error, const std::string& message) {
    return fail(error, gpu_meshing::ErrorCode::LimitExceeded, message);
}

bool finite(float value) noexcept { return std::isfinite(value); }

bool finite(matter::Float3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

float coordinate(matter::Float3 value, std::size_t axis) noexcept {
    return axis == 0u ? value.x : axis == 1u ? value.y : value.z;
}

void set_coordinate(matter::Float3& value,
                    std::size_t axis,
                    float coordinate_m) noexcept {
    if (axis == 0u) value.x = coordinate_m;
    else if (axis == 1u) value.y = coordinate_m;
    else value.z = coordinate_m;
}

bool same_float_bits(float left, float right) noexcept {
    return std::memcmp(&left, &right, sizeof(float)) == 0;
}

bool checked_add(std::uint64_t left,
                 std::uint64_t right,
                 std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

bool checked_multiply(std::uint64_t left,
                      std::uint64_t right,
                      std::uint64_t& result) noexcept {
    if (left != 0u &&
        right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

std::uint64_t digest_bytes(const std::uint8_t* bytes,
                           std::size_t size) noexcept {
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (std::size_t index = 0u; index != size; ++index) {
        digest ^= bytes[index];
        digest *= UINT64_C(1099511628211);
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
        if (size == 0u) return true;
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }
    bool string(const std::string& value) {
        return value.size() <= kMaxSectionIdBytes &&
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
        : current_(data), remaining_(size) {}

    bool u8(std::uint8_t& value) {
        if (remaining_ < 1u) return false;
        value = *current_++;
        --remaining_;
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
    bool raw(std::vector<std::uint8_t>& value, std::size_t size) {
        if (size > remaining_) return false;
        if (size == 0u) {
            value.clear();
            return true;
        }
        value.assign(current_, current_ + size);
        current_ += size;
        remaining_ -= size;
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t size = 0u;
        if (!u32(size) || size == 0u || size > kMaxSectionIdBytes ||
            size > remaining_)
            return false;
        value.assign(reinterpret_cast<const char*>(current_), size);
        current_ += size;
        remaining_ -= size;
        return true;
    }

    const std::uint8_t* current() const noexcept { return current_; }
    std::size_t remaining() const noexcept { return remaining_; }

private:
    const std::uint8_t* current_ = nullptr;
    std::size_t remaining_ = 0u;
};

bool writer_bounds(Writer& writer, const gpu_meshing::Aabb& bounds) {
    return writer.floating(bounds.min_m.x) &&
           writer.floating(bounds.min_m.y) &&
           writer.floating(bounds.min_m.z) &&
           writer.floating(bounds.max_m.x) &&
           writer.floating(bounds.max_m.y) &&
           writer.floating(bounds.max_m.z);
}

bool reader_bounds(Reader& reader, gpu_meshing::Aabb& bounds) {
    return reader.floating(bounds.min_m.x) &&
           reader.floating(bounds.min_m.y) &&
           reader.floating(bounds.min_m.z) &&
           reader.floating(bounds.max_m.x) &&
           reader.floating(bounds.max_m.y) &&
           reader.floating(bounds.max_m.z);
}

float lattice_face(float origin, float voxel, std::int64_t index) noexcept {
    return origin + voxel * static_cast<float>(index);
}

bool lattice_index(float value,
                   float origin,
                   float voxel,
                   bool upper,
                   std::int64_t& index) noexcept {
    constexpr double kInt64Minimum = -0x1p63;
    constexpr double kInt64Limit = 0x1p63;
    const double relative =
        (static_cast<double>(value) - static_cast<double>(origin)) /
        static_cast<double>(voxel);
    if (!std::isfinite(relative)) return false;
    const double nearest = std::round(relative);
    if (nearest >= kInt64Minimum && nearest < kInt64Limit) {
        const auto candidate = static_cast<std::int64_t>(nearest);
        if (same_float_bits(value, lattice_face(origin, voxel, candidate))) {
            index = candidate;
            return true;
        }
    }
    const double outward = upper ? std::ceil(relative) : std::floor(relative);
    if (!std::isfinite(outward) || outward < kInt64Minimum ||
        outward >= kInt64Limit)
        return false;
    index = static_cast<std::int64_t>(outward);
    return true;
}

bool snap_bounds_to_lattice(
    const gpu_meshing::Aabb& input,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    gpu_meshing::Aabb& snapped) noexcept {
    snapped = {};
    for (std::size_t axis = 0u; axis != 3u; ++axis) {
        std::int64_t minimum = 0;
        std::int64_t maximum = 0;
        const float origin = coordinate(lattice.origin_m, axis);
        if (!lattice_index(coordinate(input.min_m, axis), origin,
                           lattice.voxel_m, false, minimum) ||
            !lattice_index(coordinate(input.max_m, axis), origin,
                           lattice.voxel_m, true, maximum) ||
            maximum <= minimum)
            return false;
        const float minimum_face =
            lattice_face(origin, lattice.voxel_m, minimum);
        const float maximum_face =
            lattice_face(origin, lattice.voxel_m, maximum);
        if (!finite(minimum_face) || !finite(maximum_face) ||
            maximum_face <= minimum_face)
            return false;
        set_coordinate(snapped.min_m, axis, minimum_face);
        set_coordinate(snapped.max_m, axis, maximum_face);
    }
    return true;
}

bool exact_lattice_bounds(
    const gpu_meshing::Aabb& bounds,
    const gpu_meshing::ParticleSamplingLattice& lattice) noexcept {
    for (std::size_t axis = 0u; axis != 3u; ++axis) {
        const float origin = coordinate(lattice.origin_m, axis);
        std::int64_t minimum = 0;
        std::int64_t maximum = 0;
        if (!lattice_index(coordinate(bounds.min_m, axis), origin,
                           lattice.voxel_m, false, minimum) ||
            !lattice_index(coordinate(bounds.max_m, axis), origin,
                           lattice.voxel_m, true, maximum) ||
            maximum <= minimum ||
            !same_float_bits(coordinate(bounds.min_m, axis),
                             lattice_face(origin, lattice.voxel_m, minimum)) ||
            !same_float_bits(coordinate(bounds.max_m, axis),
                             lattice_face(origin, lattice.voxel_m, maximum)))
            return false;
    }
    return true;
}

matter::Float3 add_scaled(matter::Float3 value,
                          matter::Float3 axis,
                          float scale) noexcept {
    value.x += axis.x * scale;
    value.y += axis.y * scale;
    value.z += axis.z * scale;
    return value;
}

float dot(matter::Float3 left, matter::Float3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

bool unit_frame(const SpillwayHandoffRecord& handoff) noexcept {
    constexpr float kTolerance = 1.0e-3f;
    const auto length_squared = [](matter::Float3 value) {
        return value.x * value.x + value.y * value.y + value.z * value.z;
    };
    return finite(handoff.lip_origin_m) && finite(handoff.tangent) &&
           finite(handoff.lateral) && finite(handoff.up) &&
           std::fabs(length_squared(handoff.tangent) - 1.0f) <= kTolerance &&
           std::fabs(length_squared(handoff.lateral) - 1.0f) <= kTolerance &&
           std::fabs(length_squared(handoff.up) - 1.0f) <= kTolerance &&
           std::fabs(dot(handoff.tangent, handoff.lateral)) <= kTolerance &&
           std::fabs(dot(handoff.tangent, handoff.up)) <= kTolerance &&
           std::fabs(dot(handoff.lateral, handoff.up)) <= kTolerance;
}

bool derive_crop_bounds(
    const SpillwayHandoffRecord& handoff,
    float support_radius_m,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    gpu_meshing::Aabb& crop_bounds) noexcept {
    gpu_meshing::Aabb unsnapped{};
    bool initialized = false;
    const float longitudinal[] = {
        -handoff.overlap_m * 0.5f, handoff.overlap_m * 0.5f};
    const float lateral[] = {-handoff.width_m * 0.5f,
                             handoff.width_m * 0.5f};
    const float vertical[] = {-handoff.channel_depth_m,
                              handoff.effective_depth_m};
    for (float along : longitudinal) {
        for (float across : lateral) {
            for (float height : vertical) {
                matter::Float3 corner = handoff.lip_origin_m;
                corner = add_scaled(corner, handoff.tangent, along);
                corner = add_scaled(corner, handoff.lateral, across);
                corner = add_scaled(corner, handoff.up, height);
                if (!finite(corner)) return false;
                if (!initialized) {
                    unsnapped = {corner, corner};
                    initialized = true;
                } else {
                    unsnapped.min_m.x = std::min(unsnapped.min_m.x, corner.x);
                    unsnapped.min_m.y = std::min(unsnapped.min_m.y, corner.y);
                    unsnapped.min_m.z = std::min(unsnapped.min_m.z, corner.z);
                    unsnapped.max_m.x = std::max(unsnapped.max_m.x, corner.x);
                    unsnapped.max_m.y = std::max(unsnapped.max_m.y, corner.y);
                    unsnapped.max_m.z = std::max(unsnapped.max_m.z, corner.z);
                }
            }
        }
    }
    unsnapped.min_m.x -= support_radius_m;
    unsnapped.min_m.y -= support_radius_m;
    unsnapped.min_m.z -= support_radius_m;
    unsnapped.max_m.x += support_radius_m;
    unsnapped.max_m.y += support_radius_m;
    unsnapped.max_m.z += support_radius_m;
    if (!finite(unsnapped.min_m) || !finite(unsnapped.max_m)) return false;
    return snap_bounds_to_lattice(unsnapped, lattice, crop_bounds);
}

bool support_intersects(const matter::Aabb& bounds,
                        matter::Float3 position,
                        float support_radius_m) noexcept {
    const auto separation = [](float value, float minimum, float maximum) {
        if (value < minimum) return static_cast<double>(minimum - value);
        if (value > maximum) return static_cast<double>(value - maximum);
        return 0.0;
    };
    const double dx = separation(position.x, bounds.minimum.x,
                                 bounds.maximum.x);
    const double dy = separation(position.y, bounds.minimum.y,
                                 bounds.maximum.y);
    const double dz = separation(position.z, bounds.minimum.z,
                                 bounds.maximum.z);
    const double radius = support_radius_m;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

bool valid_matter_bounds(const matter::Aabb& bounds) noexcept {
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.maximum.x >= bounds.minimum.x &&
           bounds.maximum.y >= bounds.minimum.y &&
           bounds.maximum.z >= bounds.minimum.z;
}

std::uint16_t quantize(float value, float minimum, float maximum) noexcept {
    const double normalized = std::max(
        0.0, std::min(1.0,
                      (static_cast<double>(value) - minimum) /
                          (static_cast<double>(maximum) - minimum)));
    return static_cast<std::uint16_t>(
        std::llround(normalized * 65535.0));
}

float dequantize(std::uint16_t value,
                 float minimum,
                 float maximum) noexcept {
    const float decoded = minimum + (maximum - minimum) *
        (static_cast<float>(value) / 65535.0f);
    // The crop's maximum face is exclusive. Rounding the final UNORM bin to
    // that face would move a retained contributor out of the source it came
    // from, so reserve the largest representable in-crop coordinate as the
    // upper decoded endpoint. Codes below it retain the ordinary monotonic
    // UNORM mapping and code zero still reconstructs the inclusive minimum.
    return std::min(decoded, std::nextafter(maximum, minimum));
}

bool append_u16(std::vector<std::uint8_t>& bytes,
                std::uint16_t value,
                std::uint64_t limit) {
    if (bytes.size() > limit - 2u) return false;
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
    return true;
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1] << 8u);
}

bool frame_range(const WaterBoundaryCaptureFrameRecord& frame,
                 std::uint64_t payload_size,
                 std::uint64_t& byte_count,
                 std::uint64_t& end) noexcept {
    if (!checked_multiply(frame.particle_count, kPositionBytes, byte_count) ||
        frame.position_byte_offset > payload_size ||
        byte_count > payload_size - frame.position_byte_offset)
        return false;
    end = frame.position_byte_offset + byte_count;
    return true;
}

bool write_body(const WaterBoundaryAnimationSource& source,
                Writer& writer) {
    if (!writer.string(source.section_id) ||
        !writer.u64(source.source_section_payload_digest) ||
        !writer.u64(source.handoff_semantic_key) ||
        !writer.floating(source.lattice.origin_m.x) ||
        !writer.floating(source.lattice.origin_m.y) ||
        !writer.floating(source.lattice.origin_m.z) ||
        !writer.floating(source.lattice.voxel_m) ||
        !writer.u32(source.lattice.version) ||
        !writer.u32(source.frames_per_second) ||
        !writer.u32(source.phase_offset_frames) ||
        !writer.floating(source.particle_radius_m) ||
        !writer.floating(source.blend_width_m) ||
        !writer_bounds(writer, source.crop_bounds_m) ||
        source.frames.size() != kFrameCount ||
        !writer.u32(static_cast<std::uint32_t>(source.frames.size())))
        return false;
    for (const WaterBoundaryCaptureFrameRecord& frame : source.frames) {
        if (!writer.u32(frame.simulation_step) ||
            !writer.u64(frame.position_byte_offset) ||
            !writer.u32(frame.particle_count) ||
            !writer.u64(frame.content_digest))
            return false;
    }
    return writer.u64(source.quantized_positions.size()) &&
           writer.raw(source.quantized_positions.data(),
                      source.quantized_positions.size());
}

bool valid_metadata(const WaterBoundaryAnimationSource& source) noexcept {
    if (source.section_id.empty() ||
        source.section_id.size() > kMaxSectionIdBytes ||
        source.source_section_payload_digest == 0u ||
        source.handoff_semantic_key == 0u ||
        !finite(source.lattice.origin_m) ||
        !finite(source.lattice.voxel_m) || source.lattice.voxel_m <= 0.0f ||
        source.lattice.version != 1u ||
        source.frames_per_second != kFramesPerSecond ||
        source.phase_offset_frames != kPhaseOffsetFrames ||
        !finite(source.particle_radius_m) ||
        source.particle_radius_m <= 0.0f ||
        !finite(source.blend_width_m) || source.blend_width_m < 0.0f ||
        !finite(source.crop_bounds_m.min_m) ||
        !finite(source.crop_bounds_m.max_m) ||
        source.crop_bounds_m.max_m.x <= source.crop_bounds_m.min_m.x ||
        source.crop_bounds_m.max_m.y <= source.crop_bounds_m.min_m.y ||
        source.crop_bounds_m.max_m.z <= source.crop_bounds_m.min_m.z ||
        !exact_lattice_bounds(source.crop_bounds_m, source.lattice))
        return false;

    const double extent_x = static_cast<double>(source.crop_bounds_m.max_m.x) -
                            source.crop_bounds_m.min_m.x;
    const double extent_y = static_cast<double>(source.crop_bounds_m.max_m.y) -
                            source.crop_bounds_m.min_m.y;
    const double extent_z = static_cast<double>(source.crop_bounds_m.max_m.z) -
                            source.crop_bounds_m.min_m.z;
    const double maximum_error =
        std::sqrt(extent_x * extent_x + extent_y * extent_y +
                  extent_z * extent_z) /
        (2.0 * 65535.0);
    return std::isfinite(maximum_error) &&
           maximum_error <=
               static_cast<double>(source.lattice.voxel_m) / 16.0;
}

bool validate_source(const WaterBoundaryAnimationSource& source,
                     gpu_meshing::Error& error) {
    if (!valid_metadata(source) || source.frames.size() != kFrameCount ||
        source.payload_digest == 0u)
        return artifact_fail(error,
                             "water boundary source metadata is invalid");
    float support_radius_m = 0.0f;
    gpu_meshing::Error support_error{};
    if (!gpu_meshing::particle_field_support_radius_m(
            source.particle_radius_m, source.blend_width_m,
            support_radius_m, support_error))
        return artifact_fail(error,
                             "water boundary source particle support is invalid");

    std::uint64_t expected_offset = 0u;
    std::uint32_t previous_step = 0u;
    for (std::size_t index = 0u; index != source.frames.size(); ++index) {
        const auto& frame = source.frames[index];
        std::uint64_t byte_count = 0u;
        std::uint64_t end = 0u;
        if ((index != 0u && frame.simulation_step <= previous_step) ||
            frame.position_byte_offset != expected_offset ||
            !frame_range(frame, source.quantized_positions.size(), byte_count,
                         end) ||
            frame.content_digest == 0u ||
            frame.content_digest != digest_bytes(
                byte_count == 0u
                    ? nullptr
                    : source.quantized_positions.data() +
                          static_cast<std::size_t>(
                              frame.position_byte_offset),
                static_cast<std::size_t>(byte_count)))
            return artifact_fail(
                error, "water boundary source frame directory or digest is invalid");
        previous_step = frame.simulation_step;
        expected_offset = end;
    }
    if (expected_offset != source.quantized_positions.size())
        return artifact_fail(error,
                             "water boundary source frame payload is invalid");

    Writer body(kMaxBodyBytes);
    if (!write_body(source, body))
        return limit_fail(error,
                          "water boundary source exceeds the 1 GiB file limit");
    if (digest_bytes(body.bytes.data(), body.bytes.size()) !=
        source.payload_digest)
        return artifact_fail(error,
                             "water boundary source payload digest is invalid");
    error = {};
    return true;
}

bool read_file(const std::filesystem::path& path,
               std::vector<std::uint8_t>& bytes,
               gpu_meshing::Error& error) {
    bytes.clear();
    std::error_code filesystem_error;
    const std::uintmax_t size =
        std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error)
        return artifact_fail(error,
                             "could not inspect water boundary source file");
    if (size > kMaxFileBytes)
        return limit_fail(error,
                          "water boundary source exceeds the 1 GiB file limit");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return artifact_fail(error,
                             "could not open water boundary source file");
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return limit_fail(error,
                          "water boundary source file allocation failed");
    }
    if (size != 0u)
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(size));
    if (!stream)
        return artifact_fail(error,
                             "could not read water boundary source file");
    return true;
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes,
                gpu_meshing::Error& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return artifact_fail(error,
                             "could not create water boundary source file");
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream)
        return artifact_fail(error,
                             "could not write water boundary source file");
    return true;
}

}  // namespace

bool water_boundary_source_contains(
    const WaterBoundaryAnimationSource& source,
    const matter::Float3& position_m) noexcept {
    return finite(position_m) &&
           position_m.x >= source.crop_bounds_m.min_m.x &&
           position_m.y >= source.crop_bounds_m.min_m.y &&
           position_m.z >= source.crop_bounds_m.min_m.z &&
           position_m.x < source.crop_bounds_m.max_m.x &&
           position_m.y < source.crop_bounds_m.max_m.y &&
           position_m.z < source.crop_bounds_m.max_m.z;
}

bool build_water_boundary_animation_source(
    const FluidParticleAnimationCapture& capture,
    std::string_view section_id,
    std::uint64_t source_section_payload_digest,
    const SpillwayHandoffRecord& handoff,
    const gpu_meshing::ParticleSamplingLattice& lattice,
    float particle_radius_m,
    float blend_width_m,
    bool upstream_endpoint,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error) {
    source = {};
    error = {};
    try {
        const std::string expected_section =
            upstream_endpoint ? handoff.upstream_section_id
                              : handoff.downstream_section_id;
        if (capture.frames.size() != kFrameCount ||
            capture.frames_per_second != kFramesPerSecond ||
            capture.phase_offset_frames != kPhaseOffsetFrames ||
            section_id.empty() || section_id.size() > kMaxSectionIdBytes ||
            section_id != expected_section ||
            source_section_payload_digest == 0u || handoff.id.empty() ||
            handoff.semantic_key == 0u ||
            handoff.semantic_key != spillway_handoff_semantic_key(handoff) ||
            !unit_frame(handoff) || !finite(handoff.overlap_m) ||
            handoff.overlap_m <= 0.0f || !finite(handoff.width_m) ||
            handoff.width_m <= 0.0f || !finite(handoff.channel_depth_m) ||
            handoff.channel_depth_m < 0.0f ||
            !finite(handoff.effective_depth_m) ||
            handoff.effective_depth_m <= 0.0f ||
            !valid_matter_bounds(handoff.temporary_dam_exclusion_bounds_m) ||
            !finite(lattice.origin_m) || !finite(lattice.voxel_m) ||
            lattice.voxel_m <= 0.0f || lattice.version != 1u)
            return artifact_fail(
                error, "water boundary capture metadata is invalid");

        float support_radius_m = 0.0f;
        gpu_meshing::Error support_error{};
        if (!gpu_meshing::particle_field_support_radius_m(
                particle_radius_m, blend_width_m, support_radius_m,
                support_error))
            return artifact_fail(error, support_error.message);

        WaterBoundaryAnimationSource candidate{};
        candidate.section_id = section_id;
        candidate.source_section_payload_digest =
            source_section_payload_digest;
        candidate.handoff_semantic_key = handoff.semantic_key;
        candidate.lattice = lattice;
        candidate.frames_per_second = capture.frames_per_second;
        candidate.phase_offset_frames = capture.phase_offset_frames;
        candidate.particle_radius_m = particle_radius_m;
        candidate.blend_width_m = blend_width_m;
        if (!derive_crop_bounds(handoff, support_radius_m, lattice,
                                candidate.crop_bounds_m) ||
            !valid_metadata(candidate))
            return artifact_fail(
                error, "water boundary crop or quantization range is invalid");

        candidate.frames.reserve(kFrameCount);
        std::uint32_t previous_step = 0u;
        for (std::size_t frame_index = 0u;
             frame_index != capture.frames.size(); ++frame_index) {
            const FluidParticleAnimationFrame& input =
                capture.frames[frame_index];
            if (frame_index != 0u && input.simulation_step <= previous_step)
                return artifact_fail(
                    error, "water boundary capture steps are not ordered");
            previous_step = input.simulation_step;

            std::vector<matter::Float3> positions;
            positions.reserve(input.positions_m.size());
            for (matter::Float3 position : input.positions_m) {
                if (!finite(position))
                    return artifact_fail(
                        error, "water boundary capture position is non-finite");
                if (!water_boundary_source_contains(candidate, position))
                    continue;
                if (upstream_endpoint &&
                    support_intersects(
                        handoff.temporary_dam_exclusion_bounds_m, position,
                        support_radius_m))
                    continue;
                positions.push_back(position);
            }
            std::sort(positions.begin(), positions.end(),
                      [](matter::Float3 left, matter::Float3 right) {
                          if (left.x != right.x) return left.x < right.x;
                          if (left.y != right.y) return left.y < right.y;
                          return left.z < right.z;
                      });
            if (positions.size() > std::numeric_limits<std::uint32_t>::max())
                return limit_fail(
                    error, "water boundary frame particle count exceeds uint32");
            WaterBoundaryCaptureFrameRecord frame{};
            frame.simulation_step = input.simulation_step;
            frame.position_byte_offset = candidate.quantized_positions.size();
            frame.particle_count =
                static_cast<std::uint32_t>(positions.size());
            for (matter::Float3 position : positions) {
                if (!append_u16(candidate.quantized_positions,
                                quantize(position.x,
                                         candidate.crop_bounds_m.min_m.x,
                                         candidate.crop_bounds_m.max_m.x),
                                kMaxBodyBytes) ||
                    !append_u16(candidate.quantized_positions,
                                quantize(position.y,
                                         candidate.crop_bounds_m.min_m.y,
                                         candidate.crop_bounds_m.max_m.y),
                                kMaxBodyBytes) ||
                    !append_u16(candidate.quantized_positions,
                                quantize(position.z,
                                         candidate.crop_bounds_m.min_m.z,
                                         candidate.crop_bounds_m.max_m.z),
                                kMaxBodyBytes))
                    return limit_fail(
                        error,
                        "water boundary source exceeds the 1 GiB file limit");
            }
            const std::size_t byte_count = positions.size() * kPositionBytes;
            frame.content_digest = digest_bytes(
                byte_count == 0u
                    ? nullptr
                    : candidate.quantized_positions.data() +
                          static_cast<std::size_t>(
                              frame.position_byte_offset),
                byte_count);
            candidate.frames.push_back(frame);
        }

        Writer body(kMaxBodyBytes);
        if (!write_body(candidate, body))
            return limit_fail(
                error, "water boundary source exceeds the 1 GiB file limit");
        candidate.payload_digest =
            digest_bytes(body.bytes.data(), body.bytes.size());
        if (!validate_source(candidate, error)) return false;
        source = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        source = {};
        return limit_fail(error,
                          "water boundary source allocation failed");
    }
}

bool decode_water_boundary_frame(
    const WaterBoundaryAnimationSource& source,
    std::uint32_t frame_index,
    std::vector<matter::Float3>& positions_m,
    gpu_meshing::Error& error) {
    positions_m.clear();
    error = {};
    if (!valid_metadata(source) || source.payload_digest == 0u ||
        frame_index >= source.frames.size())
        return artifact_fail(error,
                             "water boundary frame index or metadata is invalid");
    const auto& frame = source.frames[frame_index];
    std::uint64_t byte_count = 0u;
    std::uint64_t end = 0u;
    if (!frame_range(frame, source.quantized_positions.size(), byte_count,
                     end) ||
        frame.content_digest == 0u ||
        frame.content_digest != digest_bytes(
            byte_count == 0u
                ? nullptr
                : source.quantized_positions.data() +
                      static_cast<std::size_t>(frame.position_byte_offset),
            static_cast<std::size_t>(byte_count)))
        return artifact_fail(error,
                             "water boundary requested frame is corrupt");
    try {
        positions_m.reserve(frame.particle_count);
        const std::uint8_t* current = frame.particle_count == 0u
            ? nullptr
            : source.quantized_positions.data() +
                  static_cast<std::size_t>(frame.position_byte_offset);
        for (std::uint32_t particle = 0u;
             particle != frame.particle_count; ++particle) {
            positions_m.push_back({
                dequantize(read_u16(current), source.crop_bounds_m.min_m.x,
                           source.crop_bounds_m.max_m.x),
                dequantize(read_u16(current + 2u),
                           source.crop_bounds_m.min_m.y,
                           source.crop_bounds_m.max_m.y),
                dequantize(read_u16(current + 4u),
                           source.crop_bounds_m.min_m.z,
                           source.crop_bounds_m.max_m.z),
            });
            current += kPositionBytes;
        }
    } catch (const std::bad_alloc&) {
        positions_m.clear();
        return limit_fail(error,
                          "water boundary frame decode allocation failed");
    }
    return true;
}

bool serialize_water_boundary_animation_source(
    const WaterBoundaryAnimationSource& source,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error) {
    bytes.clear();
    if (!validate_source(source, error)) return false;
    try {
        Writer body(kMaxBodyBytes);
        if (!write_body(source, body))
            return limit_fail(
                error, "water boundary source exceeds the 1 GiB file limit");
        std::uint64_t file_size = 0u;
        if (!checked_add(kHeaderBytes, body.bytes.size(), file_size) ||
            file_size > kMaxFileBytes)
            return limit_fail(
                error, "water boundary source exceeds the 1 GiB file limit");
        Writer writer(kMaxFileBytes);
        if (!writer.raw(kMagic, sizeof(kMagic)) || !writer.u32(kVersion) ||
            !writer.u32(kHeaderBytes) || !writer.u64(file_size) ||
            !writer.u64(source.payload_digest) ||
            !writer.raw(body.bytes.data(), body.bytes.size()))
            return limit_fail(
                error, "water boundary source exceeds the 1 GiB file limit");
        bytes = std::move(writer.bytes);
        error = {};
        return true;
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return limit_fail(error,
                          "water boundary serialization allocation failed");
    }
}

bool deserialize_water_boundary_animation_source(
    const std::vector<std::uint8_t>& bytes,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error) {
    source = {};
    error = {};
    if (bytes.size() > kMaxFileBytes)
        return limit_fail(error,
                          "water boundary source exceeds the 1 GiB file limit");
    if (bytes.size() < kHeaderBytes)
        return artifact_fail(error,
                             "water boundary source header is truncated");
    try {
        Reader header(bytes.data(), kHeaderBytes);
        std::uint8_t magic[8]{};
        for (std::uint8_t& byte : magic)
            if (!header.u8(byte))
                return artifact_fail(
                    error, "water boundary source header is truncated");
        std::uint32_t version = 0u;
        std::uint32_t header_bytes = 0u;
        std::uint64_t declared_size = 0u;
        std::uint64_t expected_digest = 0u;
        if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
            !header.u32(version) || !header.u32(header_bytes) ||
            !header.u64(declared_size) || !header.u64(expected_digest) ||
            version != kVersion || header_bytes != kHeaderBytes)
            return artifact_fail(error,
                                 "water boundary source header is invalid");
        if (declared_size > kMaxFileBytes)
            return limit_fail(
                error, "water boundary source exceeds the 1 GiB file limit");
        if (declared_size != bytes.size() || expected_digest == 0u)
            return artifact_fail(
                error, "water boundary source file size or digest is invalid");

        const std::uint8_t* body_bytes = bytes.data() + kHeaderBytes;
        const std::size_t body_size = bytes.size() - kHeaderBytes;
        if (digest_bytes(body_bytes, body_size) != expected_digest)
            return artifact_fail(error,
                                 "water boundary source payload digest is invalid");
        Reader reader(body_bytes, body_size);
        WaterBoundaryAnimationSource candidate{};
        std::uint32_t frame_count = 0u;
        if (!reader.string(candidate.section_id) ||
            !reader.u64(candidate.source_section_payload_digest) ||
            !reader.u64(candidate.handoff_semantic_key) ||
            !reader.floating(candidate.lattice.origin_m.x) ||
            !reader.floating(candidate.lattice.origin_m.y) ||
            !reader.floating(candidate.lattice.origin_m.z) ||
            !reader.floating(candidate.lattice.voxel_m) ||
            !reader.u32(candidate.lattice.version) ||
            !reader.u32(candidate.frames_per_second) ||
            !reader.u32(candidate.phase_offset_frames) ||
            !reader.floating(candidate.particle_radius_m) ||
            !reader.floating(candidate.blend_width_m) ||
            !reader_bounds(reader, candidate.crop_bounds_m) ||
            !reader.u32(frame_count) || frame_count != kFrameCount ||
            reader.remaining() < kFrameCount * kFrameRecordBytes)
            return artifact_fail(
                error, "water boundary source metadata or directory is invalid");
        candidate.frames.resize(kFrameCount);
        for (auto& frame : candidate.frames) {
            if (!reader.u32(frame.simulation_step) ||
                !reader.u64(frame.position_byte_offset) ||
                !reader.u32(frame.particle_count) ||
                !reader.u64(frame.content_digest))
                return artifact_fail(
                    error, "water boundary source frame directory is truncated");
        }
        std::uint64_t payload_size = 0u;
        if (!reader.u64(payload_size) || payload_size > kMaxBodyBytes ||
            payload_size != reader.remaining() ||
            payload_size > std::numeric_limits<std::size_t>::max() ||
            !reader.raw(candidate.quantized_positions,
                        static_cast<std::size_t>(payload_size)) ||
            reader.remaining() != 0u)
            return artifact_fail(
                error, "water boundary source frame payload is invalid");
        candidate.payload_digest = expected_digest;
        if (!validate_source(candidate, error)) return false;
        source = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        source = {};
        return limit_fail(error,
                          "water boundary deserialization allocation failed");
    }
}

bool save_water_boundary_animation_source_immutable(
    const std::filesystem::path& path,
    const WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_water_boundary_animation_source(source, bytes, error))
        return false;
    std::error_code filesystem_error;
    if (std::filesystem::exists(path, filesystem_error)) {
        std::vector<std::uint8_t> installed;
        if (!read_file(path, installed, error)) return false;
        if (installed == bytes) {
            error = {};
            return true;
        }
        return artifact_fail(
            error,
            "immutable water boundary source already has different content");
    }
    if (filesystem_error)
        return artifact_fail(error,
                             "could not inspect water boundary source path");
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystem_error);
        if (filesystem_error)
            return artifact_fail(
                error, "could not create water boundary source directory");
    }
    static std::atomic<std::uint64_t> serial{0u};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++serial);
    if (!write_file(temporary, bytes, error)) return false;
    WaterBoundaryAnimationSource reopened{};
    if (!load_water_boundary_animation_source(temporary, reopened, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    std::error_code link_error;
    std::filesystem::create_hard_link(temporary, path, link_error);
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    if (!link_error) {
        error = {};
        return true;
    }
    filesystem_error.clear();
    if (std::filesystem::exists(path, filesystem_error)) {
        std::vector<std::uint8_t> installed;
        if (!read_file(path, installed, error)) return false;
        if (installed == bytes) {
            error = {};
            return true;
        }
        return artifact_fail(
            error,
            "immutable water boundary source already has different content");
    }
    return artifact_fail(error,
                         "could not install immutable water boundary source");
}

bool load_water_boundary_animation_source(
    const std::filesystem::path& path,
    WaterBoundaryAnimationSource& source,
    gpu_meshing::Error& error) {
    source = {};
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) return false;
    return deserialize_water_boundary_animation_source(bytes, source, error);
}

}  // namespace hydrology
