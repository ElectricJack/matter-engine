#include "hydrology/hydrology_artifact.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydrology {
namespace {

constexpr std::uint8_t kMagic[8] = {'M', 'H', 'Y', 'D', 'M', 'S', 'H', '2'};
constexpr std::uint32_t kVersion = 2u;
constexpr std::uint64_t kMaxPayloadBytes = 512ull * 1024ull * 1024ull;
constexpr std::size_t kHeaderBytes = 28u;

bool fail(gpu_meshing::Error& error, const char* message) {
    error.code = gpu_meshing::ErrorCode::ArtifactFailure;
    error.message = message;
    return false;
}

class Writer {
public:
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void floating(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void f64(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }
    void raw(const std::uint8_t* data, std::size_t size) {
        bytes.insert(bytes.end(), data, data + size);
    }
    std::vector<std::uint8_t> bytes;
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
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool floating(float& value) {
        std::uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool f64(double& value) {
        std::uint64_t bits = 0;
        if (!u64(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    const std::uint8_t* current = nullptr;
    std::size_t remaining = 0;
};

std::uint64_t digest_bytes(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t digest = 1469598103934665603ull;
    for (std::size_t i = 0; i != size; ++i) {
        digest ^= bytes[i];
        digest *= 1099511628211ull;
    }
    return digest == 0 ? 1u : digest;
}

bool validate_mesh(const gpu_meshing::MeshResult& mesh) {
    if (mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() % 3u != 0u || mesh.indices.size() % 3u != 0u)
        return false;
    if (mesh.positions.size() >
            kMaxPayloadBytes / sizeof(float) ||
        mesh.indices.size() >
            kMaxPayloadBytes / sizeof(std::uint32_t))
        return false;
    for (float value : mesh.positions)
        if (!std::isfinite(value)) return false;
    for (float value : mesh.normals)
        if (!std::isfinite(value)) return false;
    const std::size_t vertex_count = mesh.positions.size() / 3u;
    for (std::uint32_t index : mesh.indices)
        if (index >= vertex_count) return false;
    return mesh.content_digest == gpu_meshing::mesh_content_digest(mesh);
}

void write_float_array(Writer& writer, const std::vector<float>& values) {
    writer.u64(static_cast<std::uint64_t>(values.size()) * sizeof(float));
    for (float value : values) writer.floating(value);
}

void write_uint_array(Writer& writer,
                      const std::vector<std::uint32_t>& values) {
    writer.u64(static_cast<std::uint64_t>(values.size()) *
               sizeof(std::uint32_t));
    for (std::uint32_t value : values) writer.u32(value);
}

void write_mesh(Writer& writer, const gpu_meshing::MeshResult& mesh) {
    writer.u32(mesh.material);
    writer.u64(mesh.content_digest);
    write_float_array(writer, mesh.positions);
    write_float_array(writer, mesh.normals);
    write_uint_array(writer, mesh.indices);
}

bool read_float_array(Reader& reader, std::vector<float>& values) {
    std::uint64_t bytes = 0;
    if (!reader.u64(bytes) || bytes > kMaxPayloadBytes || bytes % 4u != 0u ||
        bytes > reader.remaining)
        return false;
    values.resize(static_cast<std::size_t>(bytes / 4u));
    for (float& value : values)
        if (!reader.floating(value)) return false;
    return true;
}

bool read_uint_array(Reader& reader, std::vector<std::uint32_t>& values) {
    std::uint64_t bytes = 0;
    if (!reader.u64(bytes) || bytes > kMaxPayloadBytes || bytes % 4u != 0u ||
        bytes > reader.remaining)
        return false;
    values.resize(static_cast<std::size_t>(bytes / 4u));
    for (std::uint32_t& value : values)
        if (!reader.u32(value)) return false;
    return true;
}

bool read_mesh(Reader& reader, gpu_meshing::MeshResult& mesh) {
    return reader.u32(mesh.material) && reader.u64(mesh.content_digest) &&
           read_float_array(reader, mesh.positions) &&
           read_float_array(reader, mesh.normals) &&
           read_uint_array(reader, mesh.indices) && validate_mesh(mesh);
}

bool validate_artifact(const HydrologyArtifact& artifact) {
    if (artifact.product_keys.visual == 0u ||
        artifact.product_keys.coarse_cpu == 0u ||
        artifact.product_keys.gameplay == 0u ||
        artifact.semantic_key == 0u || !artifact.accepted ||
        artifact.particle_snapshot_digest == 0u ||
        !std::isfinite(artifact.particle_radius_m) ||
        artifact.particle_radius_m <= 0.0f ||
        !validate_mesh(artifact.visual_mesh) ||
        !validate_mesh(artifact.coarse_cpu_mesh) ||
        artifact.visual_mesh.material != 4u ||
        artifact.coarse_cpu_mesh.material != 4u)
        return false;
    if (!std::isfinite(artifact.stats.wall_seconds) || artifact.stats.wall_seconds < 0.0 ||
        artifact.stats.active_particles != artifact.particles.size() ||
        artifact.stats.peak_particles < artifact.stats.active_particles ||
        !artifact.sensor.complete ||
        !std::isfinite(artifact.sensor.wet_fraction) ||
        !std::isfinite(artifact.sensor.maximum_wet_fraction) ||
        !std::isfinite(artifact.sensor.final_wet_fraction) ||
        !std::isfinite(artifact.sensor.stable_window_wet_fraction) ||
        artifact.sensor.wet_fraction < 0.0f || artifact.sensor.wet_fraction > 1.0f ||
        artifact.sensor.maximum_wet_fraction < artifact.sensor.wet_fraction ||
        artifact.sensor.maximum_wet_fraction > 1.0f ||
        artifact.sensor.final_wet_fraction != artifact.sensor.wet_fraction ||
        artifact.sensor.stable_window_wet_fraction < 0.0f ||
        artifact.sensor.stable_window_wet_fraction > 1.0f ||
        !std::isfinite(artifact.gameplay_layout.origin_m.x) ||
        !std::isfinite(artifact.gameplay_layout.origin_m.y) ||
        !std::isfinite(artifact.gameplay_layout.origin_m.z) ||
        !std::isfinite(artifact.gameplay_layout.cell_size_m) ||
        artifact.gameplay_layout.cell_size_m <= 0.0f ||
        artifact.gameplay_layout.width == 0u || artifact.gameplay_layout.depth == 0u ||
        artifact.gameplay_field.size() !=
            static_cast<std::size_t>(artifact.gameplay_layout.width) *
                artifact.gameplay_layout.depth)
        return false;
    std::uint64_t previous_id = 0;
    bool have_previous_id = false;
    for (const FluidParticle& particle : artifact.particles) {
        if (!std::isfinite(particle.position_m.x) ||
            !std::isfinite(particle.position_m.y) ||
            !std::isfinite(particle.position_m.z) ||
            !std::isfinite(particle.velocity_mps.x) ||
            !std::isfinite(particle.velocity_mps.y) ||
            !std::isfinite(particle.velocity_mps.z))
            return false;
        if (have_previous_id && previous_id >= particle.id) return false;
        previous_id = particle.id;
        have_previous_id = true;
    }
    if (artifact.gameplay_field.size() > kMaxPayloadBytes / 21u)
        return false;
    if (artifact.particles.size() > kMaxPayloadBytes / 32u) return false;
    for (const GameplaySample& sample : artifact.gameplay_field) {
        if (!std::isfinite(sample.height_m) ||
            !std::isfinite(sample.depth_m) ||
            !std::isfinite(sample.velocity_x_mps) ||
            !std::isfinite(sample.velocity_y_mps) ||
            !std::isfinite(sample.velocity_z_mps))
            return false;
    }
    return true;
}

bool read_file(const std::filesystem::path& path,
               std::vector<std::uint8_t>& bytes,
               gpu_meshing::Error& error) {
    std::error_code filesystem_error;
    const std::uintmax_t size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size < kHeaderBytes ||
        size > kHeaderBytes + kMaxPayloadBytes)
        return fail(error, "hydrology artifact file size is invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(error, "could not open hydrology artifact");
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size())
        return fail(error, "could not read complete hydrology artifact");
    return true;
}

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& target) {
#ifdef _WIN32
    return MoveFileExW(source.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(source.c_str(), target.c_str()) == 0;
#endif
}

}  // namespace

bool serialize_artifact(const HydrologyArtifact& artifact,
                        std::vector<std::uint8_t>& bytes,
                        gpu_meshing::Error& error) {
    bytes.clear();
    error = {};
    if (!validate_artifact(artifact))
        return fail(error, "hydrology artifact products are invalid");
    Writer payload;
    payload.u64(artifact.product_keys.visual);
    payload.u64(artifact.product_keys.coarse_cpu);
    payload.u64(artifact.product_keys.gameplay);
    payload.u64(artifact.semantic_key);
    payload.u64(artifact.particle_snapshot_digest);
    payload.floating(artifact.particle_radius_m);
    payload.u8(artifact.accepted ? 1u : 0u);
    payload.u32(artifact.provenance.gpu_vendor);
    payload.u32(artifact.provenance.gpu_device);
    payload.u32(artifact.provenance.driver_version);
    payload.u64(artifact.provenance.physx_sdk_version);
    payload.u64(artifact.provenance.adapter_version);
    payload.u32(artifact.stats.simulated_steps);
    payload.u32(artifact.stats.active_particles);
    payload.u32(artifact.stats.peak_particles);
    payload.u32(artifact.stats.escaped_particles);
    payload.u32(artifact.stats.non_finite_particles);
    payload.f64(artifact.stats.wall_seconds);
    payload.floating(artifact.sensor.wet_fraction);
    payload.u32(artifact.sensor.stable_steps);
    payload.u32(artifact.sensor.completion_step);
    payload.u8(artifact.sensor.complete ? 1u : 0u);
    payload.floating(artifact.sensor.maximum_wet_fraction);
    payload.floating(artifact.sensor.final_wet_fraction);
    payload.floating(artifact.sensor.stable_window_wet_fraction);
    payload.u32(artifact.sensor.first_satisfied_step);
    payload.u64(static_cast<std::uint64_t>(artifact.particles.size()) * 32u);
    for (const FluidParticle& particle : artifact.particles) {
        payload.floating(particle.position_m.x); payload.floating(particle.position_m.y);
        payload.floating(particle.position_m.z); payload.floating(particle.velocity_mps.x);
        payload.floating(particle.velocity_mps.y); payload.floating(particle.velocity_mps.z);
        payload.u64(particle.id);
    }
    write_mesh(payload, artifact.visual_mesh);
    write_mesh(payload, artifact.coarse_cpu_mesh);
    payload.floating(artifact.gameplay_layout.origin_m.x);
    payload.floating(artifact.gameplay_layout.origin_m.y);
    payload.floating(artifact.gameplay_layout.origin_m.z);
    payload.floating(artifact.gameplay_layout.cell_size_m);
    payload.u32(artifact.gameplay_layout.width);
    payload.u32(artifact.gameplay_layout.depth);
    payload.u64(static_cast<std::uint64_t>(artifact.gameplay_field.size()) *
                21u);
    for (const GameplaySample& sample : artifact.gameplay_field) {
        payload.floating(sample.height_m);
        payload.floating(sample.depth_m);
        payload.floating(sample.velocity_x_mps);
        payload.floating(sample.velocity_y_mps);
        payload.floating(sample.velocity_z_mps);
        payload.u8(sample.wet_valid ? 1u : 0u);
    }
    if (payload.bytes.size() > kMaxPayloadBytes)
        return fail(error, "hydrology artifact exceeds the payload limit");
    const std::uint64_t digest =
        digest_bytes(payload.bytes.data(), payload.bytes.size());
    Writer file;
    file.raw(kMagic, sizeof(kMagic));
    file.u32(kVersion);
    file.u64(payload.bytes.size());
    file.u64(digest);
    file.raw(payload.bytes.data(), payload.bytes.size());
    bytes = std::move(file.bytes);
    return true;
}

bool deserialize_artifact(const std::vector<std::uint8_t>& bytes,
                          HydrologyArtifact& artifact,
                          gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    if (bytes.size() < kHeaderBytes)
        return fail(error, "hydrology artifact is truncated");
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return fail(error, "hydrology artifact magic is invalid");
    Reader header(bytes.data() + sizeof(kMagic), bytes.size() - sizeof(kMagic));
    std::uint32_t version = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t expected_digest = 0;
    if (!header.u32(version) || !header.u64(payload_size) ||
        !header.u64(expected_digest) || version != kVersion ||
        payload_size > kMaxPayloadBytes ||
        payload_size != bytes.size() - kHeaderBytes)
        return fail(error, "hydrology artifact header is invalid");
    const std::uint8_t* payload = bytes.data() + kHeaderBytes;
    if (digest_bytes(payload, static_cast<std::size_t>(payload_size)) !=
        expected_digest)
        return fail(error, "hydrology artifact payload digest is invalid");
    Reader reader(payload, static_cast<std::size_t>(payload_size));
    HydrologyArtifact candidate{};
    std::uint8_t accepted = 0;
    std::uint8_t sensor_complete = 0;
    if (!reader.u64(candidate.product_keys.visual) ||
        !reader.u64(candidate.product_keys.coarse_cpu) ||
        !reader.u64(candidate.product_keys.gameplay) ||
        !reader.u64(candidate.semantic_key) ||
        !reader.u64(candidate.particle_snapshot_digest) ||
        !reader.floating(candidate.particle_radius_m) ||
        !reader.u8(accepted) || accepted > 1u ||
        !reader.u32(candidate.provenance.gpu_vendor) ||
        !reader.u32(candidate.provenance.gpu_device) ||
        !reader.u32(candidate.provenance.driver_version) ||
        !reader.u64(candidate.provenance.physx_sdk_version) ||
        !reader.u64(candidate.provenance.adapter_version) ||
        !reader.u32(candidate.stats.simulated_steps) ||
        !reader.u32(candidate.stats.active_particles) ||
        !reader.u32(candidate.stats.peak_particles) ||
        !reader.u32(candidate.stats.escaped_particles) ||
        !reader.u32(candidate.stats.non_finite_particles) ||
        !reader.f64(candidate.stats.wall_seconds) ||
        !reader.floating(candidate.sensor.wet_fraction) ||
        !reader.u32(candidate.sensor.stable_steps) ||
        !reader.u32(candidate.sensor.completion_step) ||
        !reader.u8(sensor_complete) || sensor_complete > 1u ||
        !reader.floating(candidate.sensor.maximum_wet_fraction) ||
        !reader.floating(candidate.sensor.final_wet_fraction) ||
        !reader.floating(candidate.sensor.stable_window_wet_fraction) ||
        !reader.u32(candidate.sensor.first_satisfied_step))
        return fail(error, "hydrology artifact product payload is invalid");
    candidate.accepted = accepted != 0u;
    candidate.sensor.complete = sensor_complete != 0u;
    std::uint64_t particle_bytes = 0;
    if (!reader.u64(particle_bytes) || particle_bytes > kMaxPayloadBytes ||
        particle_bytes % 32u != 0u || particle_bytes > reader.remaining)
        return fail(error, "hydrology artifact particle payload is invalid");
    candidate.particles.resize(static_cast<std::size_t>(particle_bytes / 32u));
    for (FluidParticle& particle : candidate.particles) {
        if (!reader.floating(particle.position_m.x) || !reader.floating(particle.position_m.y) ||
            !reader.floating(particle.position_m.z) || !reader.floating(particle.velocity_mps.x) ||
            !reader.floating(particle.velocity_mps.y) || !reader.floating(particle.velocity_mps.z) ||
            !reader.u64(particle.id))
            return fail(error, "hydrology artifact particle payload is invalid");
    }
    if (!read_mesh(reader, candidate.visual_mesh) ||
        !read_mesh(reader, candidate.coarse_cpu_mesh) ||
        !reader.floating(candidate.gameplay_layout.origin_m.x) ||
        !reader.floating(candidate.gameplay_layout.origin_m.y) ||
        !reader.floating(candidate.gameplay_layout.origin_m.z) ||
        !reader.floating(candidate.gameplay_layout.cell_size_m) ||
        !reader.u32(candidate.gameplay_layout.width) ||
        !reader.u32(candidate.gameplay_layout.depth))
        return fail(error, "hydrology artifact product payload is invalid");
    std::uint64_t gameplay_bytes = 0;
    if (!reader.u64(gameplay_bytes) || gameplay_bytes > kMaxPayloadBytes ||
        gameplay_bytes % 21u != 0u || gameplay_bytes > reader.remaining)
        return fail(error, "hydrology artifact gameplay payload is invalid");
    candidate.gameplay_field.resize(
        static_cast<std::size_t>(gameplay_bytes / 21u));
    for (GameplaySample& sample : candidate.gameplay_field) {
        std::uint8_t wet = 0;
        if (!reader.floating(sample.height_m) ||
            !reader.floating(sample.depth_m) ||
            !reader.floating(sample.velocity_x_mps) ||
            !reader.floating(sample.velocity_y_mps) ||
            !reader.floating(sample.velocity_z_mps) || !reader.u8(wet) ||
            wet > 1u)
            return fail(error, "hydrology gameplay sample is invalid");
        sample.wet_valid = wet != 0u;
    }
    candidate.payload_digest = expected_digest;
    if (reader.remaining != 0u || !validate_artifact(candidate))
        return fail(error, "hydrology artifact has trailing or invalid data");
    artifact = std::move(candidate);
    return true;
}

bool save_artifact_atomic(const std::filesystem::path& path,
                          const HydrologyArtifact& artifact,
                          gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_artifact(artifact, bytes, error)) return false;
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystem_error);
        if (filesystem_error)
            return fail(error, "could not create hydrology artifact directory");
    }
    static std::atomic<std::uint64_t> serial{0};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++serial);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return fail(error, "could not create hydrology artifact temporary");
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return fail(error, "could not write hydrology artifact temporary");
        }
    }
    HydrologyArtifact reopened{};
    if (!load_artifact_validated(temporary, artifact.product_keys.visual,
                                 reopened, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    if (!replace_file(temporary, path)) {
        std::filesystem::remove(temporary, filesystem_error);
        return fail(error, "could not atomically replace hydrology artifact");
    }
    return true;
}

bool load_artifact_validated(const std::filesystem::path& path,
                             std::uint64_t expected_visual_key,
                             HydrologyArtifact& artifact,
                             gpu_meshing::Error& error,
                             std::uint64_t expected_semantic_key) {
    artifact = {};
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error) ||
        !deserialize_artifact(bytes, artifact, error))
        return false;
    if (artifact.product_keys.visual != expected_visual_key ||
        (expected_semantic_key != 0u && artifact.semantic_key != expected_semantic_key)) {
        artifact = {};
        return fail(error, "hydrology artifact semantic key is stale");
    }
    return true;
}

bool load_or_build_artifact(const std::filesystem::path& path,
                            std::uint64_t expected_visual_key,
                            const ArtifactBuilder& builder,
                            HydrologyArtifact& artifact,
                            gpu_meshing::Error& error,
                            std::uint64_t expected_semantic_key) {
    if (std::filesystem::exists(path) &&
        load_artifact_validated(path, expected_visual_key, artifact, error,
                                expected_semantic_key))
        return true;
    artifact = {};
    error = {};
    if (!builder)
        return fail(error, "hydrology artifact builder is unavailable");
    HydrologyArtifact candidate{};
    if (!builder(candidate, error)) return false;
    if (candidate.product_keys.visual != expected_visual_key ||
        (expected_semantic_key != 0u && candidate.semantic_key != expected_semantic_key))
        return fail(error, "hydrology artifact builder returned the wrong key");
    if (!save_artifact_atomic(path, candidate, error)) return false;
    artifact = std::move(candidate);
    return true;
}

}  // namespace hydrology
