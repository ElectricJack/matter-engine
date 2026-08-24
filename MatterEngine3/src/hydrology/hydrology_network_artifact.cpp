#include "hydrology/hydrology_network_artifact.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydrology {
namespace {

constexpr std::uint8_t kMagic[8] = {'M', 'H', 'Y', 'D', 'N', 'E', 'T', '1'};
constexpr std::uint32_t kVersion = 1u;
constexpr std::uint64_t kMaxPayloadBytes = 16ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxStringBytes = 4096u;
constexpr std::uint32_t kMaxReferences = 4096u;
constexpr std::uint32_t kMaxDependencies = 4096u;
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
    void string(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
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
        if (remaining == 0u) return false;
        value = *current++;
        --remaining;
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0u;
        for (unsigned shift = 0; shift != 32; shift += 8) {
            std::uint8_t byte = 0u;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0u;
        for (unsigned shift = 0; shift != 64; shift += 8) {
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
        if (!u32(size) || size > kMaxStringBytes || size > remaining)
            return false;
        value.assign(reinterpret_cast<const char*>(current), size);
        current += size;
        remaining -= size;
        return true;
    }
    const std::uint8_t* current = nullptr;
    std::size_t remaining = 0u;
};

std::uint64_t digest_bytes(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (std::size_t i = 0; i < size; ++i) {
        digest ^= bytes[i];
        digest *= UINT64_C(1099511628211);
    }
    return digest == 0u ? 1u : digest;
}

bool valid_bounds(const matter::Aabb& bounds) {
    const auto finite = [](matter::Float3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    };
    return finite(bounds.minimum) && finite(bounds.maximum) &&
           bounds.minimum.x < bounds.maximum.x &&
           bounds.minimum.y < bounds.maximum.y &&
           bounds.minimum.z < bounds.maximum.z;
}

bool cache_relative_path(const std::string& authored) {
    if (authored.empty() || authored.size() > kMaxStringBytes) return false;
    const std::filesystem::path path(authored);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto& part : path) {
        if (part == ".." || part == ".") return false;
    }
    return path.lexically_normal().generic_string() == authored;
}

void canonicalize_references(
    std::vector<HydrologyArtifactReference>& references) {
    for (auto& reference : references)
        std::sort(reference.dependencies.begin(), reference.dependencies.end());
    std::sort(references.begin(), references.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.id < rhs.id;
              });
}

bool validate_references(
    const std::vector<HydrologyArtifactReference>& references,
    std::unordered_set<std::string>& ids) {
    if (references.size() > kMaxReferences) return false;
    for (const auto& reference : references) {
        if (reference.id.empty() || reference.id.size() > kMaxStringBytes ||
            !ids.insert(reference.id).second ||
            !cache_relative_path(reference.relative_path) ||
            reference.semantic_key == 0u || reference.payload_digest == 0u ||
            reference.dependencies.size() > kMaxDependencies)
            return false;
        std::unordered_set<std::string> dependencies;
        for (const auto& dependency : reference.dependencies) {
            if (dependency.empty() || dependency.size() > kMaxStringBytes ||
                !dependencies.insert(dependency).second)
                return false;
        }
    }
    return true;
}

bool validate_manifest(const HydrologyNetworkArtifact& artifact,
                       bool require_ready) {
    if (artifact.state != HydrologyNetworkState::Incomplete &&
        artifact.state != HydrologyNetworkState::Failed &&
        artifact.state != HydrologyNetworkState::Ready)
        return false;
    if (artifact.network_key == 0u || artifact.terrain_revision == 0u ||
        !valid_bounds(artifact.bounds_m) ||
        artifact.topological_order.size() > kMaxReferences)
        return false;
    if (require_ready && artifact.state != HydrologyNetworkState::Ready)
        return false;
    if (artifact.state != HydrologyNetworkState::Ready) {
        return artifact.sections.empty() && artifact.handoffs.empty() &&
               artifact.topological_order.empty();
    }

    std::unordered_set<std::string> section_ids;
    std::unordered_set<std::string> handoff_ids;
    if (!validate_references(artifact.sections, section_ids) ||
        !validate_references(artifact.handoffs, handoff_ids) ||
        artifact.sections.empty() ||
        artifact.topological_order.size() != artifact.sections.size())
        return false;

    std::unordered_map<std::string, std::size_t> order;
    for (std::size_t index = 0; index < artifact.topological_order.size();
         ++index) {
        const auto& id = artifact.topological_order[index];
        if (section_ids.find(id) == section_ids.end() ||
            !order.emplace(id, index).second)
            return false;
    }
    for (const auto& section : artifact.sections) {
        const auto position = order.find(section.id);
        if (position == order.end()) return false;
        for (const auto& dependency : section.dependencies) {
            const auto upstream = order.find(dependency);
            if (upstream == order.end() || upstream->second >= position->second)
                return false;
        }
    }
    for (const auto& handoff : artifact.handoffs) {
        for (const auto& dependency : handoff.dependencies) {
            if (section_ids.find(dependency) == section_ids.end()) return false;
        }
    }
    return true;
}

void write_reference(Writer& writer,
                     const HydrologyArtifactReference& reference) {
    writer.string(reference.id);
    writer.string(reference.relative_path);
    writer.u32(static_cast<std::uint32_t>(reference.dependencies.size()));
    for (const auto& dependency : reference.dependencies)
        writer.string(dependency);
    writer.u64(reference.semantic_key);
    writer.u64(reference.payload_digest);
}

bool read_reference(Reader& reader, HydrologyArtifactReference& reference) {
    std::uint32_t count = 0u;
    if (!reader.string(reference.id) ||
        !reader.string(reference.relative_path) || !reader.u32(count) ||
        count > kMaxDependencies)
        return false;
    reference.dependencies.resize(count);
    for (auto& dependency : reference.dependencies)
        if (!reader.string(dependency)) return false;
    return reader.u64(reference.semantic_key) &&
           reader.u64(reference.payload_digest);
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

} // namespace

bool serialize_network_artifact(const HydrologyNetworkArtifact& artifact,
                                std::vector<std::uint8_t>& bytes,
                                gpu_meshing::Error& error) {
    bytes.clear();
    error = {};
    HydrologyNetworkArtifact canonical = artifact;
    canonicalize_references(canonical.sections);
    canonicalize_references(canonical.handoffs);
    if (!validate_manifest(canonical, false))
        return fail(error, "hydrology network manifest is invalid");

    Writer payload;
    payload.u8(static_cast<std::uint8_t>(canonical.state));
    payload.u64(canonical.network_key);
    payload.u64(canonical.terrain_revision);
    for (const float value : {canonical.bounds_m.minimum.x,
                              canonical.bounds_m.minimum.y,
                              canonical.bounds_m.minimum.z,
                              canonical.bounds_m.maximum.x,
                              canonical.bounds_m.maximum.y,
                              canonical.bounds_m.maximum.z})
        payload.floating(value);
    payload.u32(static_cast<std::uint32_t>(canonical.topological_order.size()));
    for (const auto& id : canonical.topological_order) payload.string(id);
    payload.u32(static_cast<std::uint32_t>(canonical.sections.size()));
    for (const auto& reference : canonical.sections)
        write_reference(payload, reference);
    payload.u32(static_cast<std::uint32_t>(canonical.handoffs.size()));
    for (const auto& reference : canonical.handoffs)
        write_reference(payload, reference);
    if (payload.bytes.size() > kMaxPayloadBytes)
        return fail(error, "hydrology network manifest exceeds its payload limit");

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

bool deserialize_network_artifact(const std::vector<std::uint8_t>& bytes,
                                  HydrologyNetworkArtifact& artifact,
                                  gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    if (bytes.size() < kHeaderBytes ||
        std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return fail(error, "hydrology network manifest header is invalid");
    Reader header(bytes.data() + sizeof(kMagic), bytes.size() - sizeof(kMagic));
    std::uint32_t version = 0u;
    std::uint64_t payload_size = 0u;
    std::uint64_t expected_digest = 0u;
    if (!header.u32(version) || !header.u64(payload_size) ||
        !header.u64(expected_digest) || version != kVersion ||
        payload_size > kMaxPayloadBytes ||
        payload_size != bytes.size() - kHeaderBytes)
        return fail(error, "hydrology network manifest header is invalid");
    const auto* payload = bytes.data() + kHeaderBytes;
    if (digest_bytes(payload, static_cast<std::size_t>(payload_size)) !=
        expected_digest)
        return fail(error, "hydrology network manifest digest is invalid");

    Reader reader(payload, static_cast<std::size_t>(payload_size));
    HydrologyNetworkArtifact candidate{};
    std::uint8_t state = 0u;
    std::uint32_t count = 0u;
    if (!reader.u8(state) || state > static_cast<std::uint8_t>(
                                      HydrologyNetworkState::Ready) ||
        !reader.u64(candidate.network_key) ||
        !reader.u64(candidate.terrain_revision) ||
        !reader.floating(candidate.bounds_m.minimum.x) ||
        !reader.floating(candidate.bounds_m.minimum.y) ||
        !reader.floating(candidate.bounds_m.minimum.z) ||
        !reader.floating(candidate.bounds_m.maximum.x) ||
        !reader.floating(candidate.bounds_m.maximum.y) ||
        !reader.floating(candidate.bounds_m.maximum.z) ||
        !reader.u32(count) || count > kMaxReferences)
        return fail(error, "hydrology network manifest payload is invalid");
    candidate.state = static_cast<HydrologyNetworkState>(state);
    candidate.topological_order.resize(count);
    for (auto& id : candidate.topological_order)
        if (!reader.string(id))
            return fail(error, "hydrology network manifest order is invalid");
    if (!reader.u32(count) || count > kMaxReferences)
        return fail(error, "hydrology network section count is invalid");
    candidate.sections.resize(count);
    for (auto& reference : candidate.sections)
        if (!read_reference(reader, reference))
            return fail(error, "hydrology network section reference is invalid");
    if (!reader.u32(count) || count > kMaxReferences)
        return fail(error, "hydrology network handoff count is invalid");
    candidate.handoffs.resize(count);
    for (auto& reference : candidate.handoffs)
        if (!read_reference(reader, reference))
            return fail(error, "hydrology network handoff reference is invalid");
    candidate.payload_digest = expected_digest;
    canonicalize_references(candidate.sections);
    canonicalize_references(candidate.handoffs);
    if (reader.remaining != 0u || !validate_manifest(candidate, false))
        return fail(error, "hydrology network manifest has invalid data");
    artifact = std::move(candidate);
    return true;
}

bool save_network_artifact_atomic(const std::filesystem::path& path,
                                  const HydrologyNetworkArtifact& artifact,
                                  gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_network_artifact(artifact, bytes, error)) return false;
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error)
            return fail(error, "could not create hydrology network directory");
    }
    static std::atomic<std::uint64_t> serial{0u};
    const auto temporary = path.string() + ".tmp-" +
                           std::to_string(++serial);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return fail(error, "could not create hydrology network temporary");
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return fail(error, "could not write hydrology network temporary");
        }
    }
    std::vector<std::uint8_t> reopened(bytes.size());
    {
        std::ifstream stream(temporary, std::ios::binary);
        stream.read(reinterpret_cast<char*>(reopened.data()),
                    static_cast<std::streamsize>(reopened.size()));
        if (!stream) {
            std::filesystem::remove(temporary, filesystem_error);
            return fail(error, "could not reopen hydrology network temporary");
        }
    }
    HydrologyNetworkArtifact validated{};
    if (!deserialize_network_artifact(reopened, validated, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    if (!replace_file(temporary, path)) {
        std::filesystem::remove(temporary, filesystem_error);
        return fail(error, "could not publish hydrology network manifest");
    }
    return true;
}

bool load_network_artifact_validated(
    const std::filesystem::path& path,
    std::uint64_t expected_network_key,
    std::uint64_t expected_terrain_revision,
    HydrologyNetworkArtifact& artifact,
    gpu_meshing::Error& error) {
    artifact = {};
    error = {};
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size < kHeaderBytes ||
        size > kHeaderBytes + kMaxPayloadBytes)
        return fail(error, "hydrology network manifest file size is invalid");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream || !deserialize_network_artifact(bytes, artifact, error))
        return false;
    if (!validate_manifest(artifact, true) ||
        artifact.network_key != expected_network_key ||
        artifact.terrain_revision != expected_terrain_revision) {
        artifact = {};
        return fail(error, "hydrology network manifest is not ready or is stale");
    }
    return true;
}

} // namespace hydrology
