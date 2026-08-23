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
#define NOMINMAX
#include <windows.h>
#endif

namespace hydrology {
namespace {

constexpr std::uint8_t kMagic[8] = {'M', 'H', 'Y', 'D', 'M', 'S', 'H', '1'};
constexpr std::uint32_t kVersion = 1u;
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
        artifact.particle_snapshot_digest == 0u ||
        !validate_mesh(artifact.visual_mesh) ||
        !validate_mesh(artifact.coarse_cpu_mesh))
        return false;
    if (artifact.gameplay_field.size() > kMaxPayloadBytes / 21u)
        return false;
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
    payload.u64(artifact.particle_snapshot_digest);
    payload.u32(artifact.provenance.gpu_vendor);
    payload.u32(artifact.provenance.gpu_device);
    payload.u32(artifact.provenance.driver_version);
    write_mesh(payload, artifact.visual_mesh);
    write_mesh(payload, artifact.coarse_cpu_mesh);
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
    if (!reader.u64(candidate.product_keys.visual) ||
        !reader.u64(candidate.product_keys.coarse_cpu) ||
        !reader.u64(candidate.product_keys.gameplay) ||
        !reader.u64(candidate.particle_snapshot_digest) ||
        !reader.u32(candidate.provenance.gpu_vendor) ||
        !reader.u32(candidate.provenance.gpu_device) ||
        !reader.u32(candidate.provenance.driver_version) ||
        !read_mesh(reader, candidate.visual_mesh) ||
        !read_mesh(reader, candidate.coarse_cpu_mesh))
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
                             gpu_meshing::Error& error) {
    artifact = {};
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error) ||
        !deserialize_artifact(bytes, artifact, error))
        return false;
    if (artifact.product_keys.visual != expected_visual_key) {
        artifact = {};
        return fail(error, "hydrology artifact semantic key is stale");
    }
    return true;
}

bool load_or_build_artifact(const std::filesystem::path& path,
                            std::uint64_t expected_visual_key,
                            const ArtifactBuilder& builder,
                            HydrologyArtifact& artifact,
                            gpu_meshing::Error& error) {
    if (std::filesystem::exists(path) &&
        load_artifact_validated(path, expected_visual_key, artifact, error))
        return true;
    artifact = {};
    error = {};
    if (!builder)
        return fail(error, "hydrology artifact builder is unavailable");
    HydrologyArtifact candidate{};
    if (!builder(candidate, error)) return false;
    if (candidate.product_keys.visual != expected_visual_key)
        return fail(error, "hydrology artifact builder returned the wrong key");
    if (!save_artifact_atomic(path, candidate, error)) return false;
    artifact = std::move(candidate);
    return true;
}

}  // namespace hydrology
