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

constexpr std::uint8_t kMagic[8] = {'M', 'H', 'Y', 'D', 'N', 'E', 'T', '2'};
constexpr std::uint32_t kVersion = 2u;
constexpr std::uint64_t kMaxPayloadBytes = 16ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxStringBytes = 4096u;
constexpr std::uint32_t kMaxReferences = 4096u;
constexpr std::uint32_t kMaxDependencies = 4096u;
constexpr std::size_t kHeaderBytes = 28u;
constexpr std::uint8_t kFieldMagic[8] = {
    'M', 'H', 'Y', 'F', 'I', 'E', 'L', '1'};
constexpr std::uint32_t kFieldVersion = 1u;
constexpr std::uint64_t kMaxFieldPayloadBytes = 16ull * 1024ull * 1024ull;
constexpr std::size_t kFieldHeaderBytes = 32u;
constexpr std::uint64_t kFieldLayoutBytes = 24u;
constexpr std::uint64_t kGameplayRecordBytes = 21u;
constexpr std::uint64_t kPresentationRecordBytes = 22u;

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

bool valid_field_layout(const GameplayFieldLayout& layout) {
    return std::isfinite(layout.origin_m.x) &&
           std::isfinite(layout.origin_m.y) &&
           std::isfinite(layout.origin_m.z) &&
           std::isfinite(layout.cell_size_m) && layout.cell_size_m > 0.0f &&
           layout.width != 0u && layout.depth != 0u &&
           static_cast<std::uint64_t>(layout.width) * layout.depth <=
               16ull * 1024ull * 1024ull;
}

bool same_layout(const GameplayFieldLayout& lhs,
                 const GameplayFieldLayout& rhs) {
    return lhs.origin_m.x == rhs.origin_m.x &&
           lhs.origin_m.y == rhs.origin_m.y &&
           lhs.origin_m.z == rhs.origin_m.z &&
           lhs.cell_size_m == rhs.cell_size_m && lhs.width == rhs.width &&
           lhs.depth == rhs.depth;
}

std::filesystem::path network_cache_root(
    const std::filesystem::path& manifest_path) {
    const auto directory = manifest_path.parent_path();
    return directory.filename() == "hydrology"
        ? directory.parent_path() : directory;
}

bool validate_field_package(const std::filesystem::path& manifest_path,
                            const HydrologyNetworkArtifact& manifest,
                            gpu_meshing::Error& error) {
    if (manifest.state != HydrologyNetworkState::Ready) return true;
    const HydrologyFieldProductReference* runtime = nullptr;
    const HydrologyFieldProductReference* presentation = nullptr;
    for (const auto& reference : manifest.field_products) {
        if (reference.kind == HydrologyFieldProductKind::Runtime)
            runtime = &reference;
        else if (reference.kind == HydrologyFieldProductKind::Presentation)
            presentation = &reference;
    }
    if (runtime == nullptr || presentation == nullptr)
        return fail(error, "hydrology field package closure is incomplete");
    const auto cache_root = network_cache_root(manifest_path);
    HydrologyFieldProduct runtime_product{};
    HydrologyFieldProduct presentation_product{};
    if (!load_hydrology_field_product_validated(
            cache_root / runtime->relative_path,
            HydrologyFieldProductKind::Runtime,
            manifest.runtime_field_digest, runtime_product, error) ||
        !load_hydrology_field_product_validated(
            cache_root / presentation->relative_path,
            HydrologyFieldProductKind::Presentation,
            manifest.presentation_field_digest,
            presentation_product, error))
        return false;
    if (!same_layout(runtime_product.layout, presentation_product.layout) ||
        runtime_product.gameplay.size() !=
            presentation_product.presentation.size())
        return fail(error, "hydrology field package layouts do not match");
    for (std::size_t index = 0u;
         index != runtime_product.gameplay.size(); ++index)
        if (runtime_product.gameplay[index].wet_valid !=
            presentation_product.presentation[index].wet_valid)
            return fail(error, "hydrology field package wet masks do not match");
    return true;
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

void canonicalize_field_products(
    std::vector<HydrologyFieldProductReference>& products) {
    std::sort(products.begin(), products.end(),
              [](const auto& lhs, const auto& rhs) {
                  return static_cast<std::uint8_t>(lhs.kind) <
                         static_cast<std::uint8_t>(rhs.kind);
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
               artifact.topological_order.empty() &&
               artifact.runtime_field_digest == 0u &&
               artifact.presentation_field_digest == 0u &&
               artifact.field_products.empty();
    }

    if (artifact.runtime_field_digest == 0u ||
        artifact.presentation_field_digest == 0u ||
        artifact.field_products.size() != 2u)
        return false;
    bool have_runtime = false;
    bool have_presentation = false;
    std::unordered_set<std::string> field_paths;
    for (const auto& product : artifact.field_products) {
        if (!cache_relative_path(product.relative_path) ||
            !field_paths.insert(product.relative_path).second ||
            product.payload_digest == 0u)
            return false;
        switch (product.kind) {
            case HydrologyFieldProductKind::Runtime:
                if (have_runtime ||
                    product.payload_digest != artifact.runtime_field_digest)
                    return false;
                have_runtime = true;
                break;
            case HydrologyFieldProductKind::Presentation:
                if (have_presentation || product.payload_digest !=
                                             artifact.presentation_field_digest)
                    return false;
                have_presentation = true;
                break;
            default:
                return false;
        }
    }
    if (!have_runtime || !have_presentation) return false;

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

bool serialize_hydrology_field_product(
    const HydrologyFieldProduct& product,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error) {
    bytes.clear();
    error = {};
    if (!valid_field_layout(product.layout))
        return fail(error, "hydrology field layout is invalid");
    const auto count = static_cast<std::uint64_t>(product.layout.width) *
        product.layout.depth;
    std::uint64_t record_bytes = 0u;
    std::uint64_t computed_digest = 0u;
    switch (product.kind) {
        case HydrologyFieldProductKind::Runtime:
            if (!product.presentation.empty() ||
                product.gameplay.size() != count)
                return fail(error, "runtime field product type is invalid");
            record_bytes = kGameplayRecordBytes;
            computed_digest = hydrology_runtime_field_digest(
                product.layout, product.gameplay);
            break;
        case HydrologyFieldProductKind::Presentation:
            if (!product.gameplay.empty() ||
                product.presentation.size() != count)
                return fail(error, "presentation field product type is invalid");
            record_bytes = kPresentationRecordBytes;
            computed_digest = hydrology_presentation_field_digest(
                product.layout, product.presentation);
            break;
        default:
            return fail(error, "hydrology field product kind is invalid");
    }
    if (computed_digest == 0u || product.payload_digest != computed_digest ||
        count > (kMaxFieldPayloadBytes - kFieldLayoutBytes) / record_bytes)
        return fail(error, "hydrology field product digest is invalid");

    Writer payload;
    payload.floating(product.layout.origin_m.x);
    payload.floating(product.layout.origin_m.y);
    payload.floating(product.layout.origin_m.z);
    payload.floating(product.layout.cell_size_m);
    payload.u32(product.layout.width);
    payload.u32(product.layout.depth);
    if (product.kind == HydrologyFieldProductKind::Runtime) {
        for (const auto& sample : product.gameplay) {
            payload.floating(sample.height_m);
            payload.floating(sample.depth_m);
            payload.floating(sample.velocity_x_mps);
            payload.floating(sample.velocity_y_mps);
            payload.floating(sample.velocity_z_mps);
            payload.u8(sample.wet_valid ? 1u : 0u);
        }
    } else {
        for (const auto& sample : product.presentation) {
            payload.floating(sample.normal_x);
            payload.floating(sample.normal_z);
            payload.floating(sample.turbulence);
            payload.floating(sample.aeration);
            payload.floating(sample.foam_potential);
            payload.u8(static_cast<std::uint8_t>(sample.feature));
            payload.u8(sample.wet_valid ? 1u : 0u);
        }
    }
    if (payload.bytes.size() != kFieldLayoutBytes + count * record_bytes)
        return fail(error, "hydrology field product size is invalid");

    Writer file;
    file.raw(kFieldMagic, sizeof(kFieldMagic));
    file.u32(kFieldVersion);
    file.u8(static_cast<std::uint8_t>(product.kind));
    file.u8(0u);
    file.u8(0u);
    file.u8(0u);
    file.u64(payload.bytes.size());
    file.u64(computed_digest);
    file.raw(payload.bytes.data(), payload.bytes.size());
    bytes = std::move(file.bytes);
    return true;
}

bool deserialize_hydrology_field_product(
    const std::vector<std::uint8_t>& bytes,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    error = {};
    if (bytes.size() < kFieldHeaderBytes ||
        std::memcmp(bytes.data(), kFieldMagic, sizeof(kFieldMagic)) != 0)
        return fail(error, "hydrology field product header is invalid");
    Reader header(bytes.data() + sizeof(kFieldMagic),
                  bytes.size() - sizeof(kFieldMagic));
    std::uint32_t version = 0u;
    std::uint8_t kind = 0u;
    std::uint8_t reserved[3]{};
    std::uint64_t payload_size = 0u;
    std::uint64_t expected_digest = 0u;
    if (!header.u32(version) || !header.u8(kind) ||
        !header.u8(reserved[0]) || !header.u8(reserved[1]) ||
        !header.u8(reserved[2]) || !header.u64(payload_size) ||
        !header.u64(expected_digest) || version != kFieldVersion ||
        kind > static_cast<std::uint8_t>(
                   HydrologyFieldProductKind::Presentation) ||
        reserved[0] != 0u || reserved[1] != 0u || reserved[2] != 0u ||
        expected_digest == 0u || payload_size > kMaxFieldPayloadBytes ||
        payload_size != bytes.size() - kFieldHeaderBytes)
        return fail(error, "hydrology field product header is invalid");

    HydrologyFieldProduct candidate{};
    candidate.kind = static_cast<HydrologyFieldProductKind>(kind);
    Reader reader(bytes.data() + kFieldHeaderBytes,
                  static_cast<std::size_t>(payload_size));
    if (!reader.floating(candidate.layout.origin_m.x) ||
        !reader.floating(candidate.layout.origin_m.y) ||
        !reader.floating(candidate.layout.origin_m.z) ||
        !reader.floating(candidate.layout.cell_size_m) ||
        !reader.u32(candidate.layout.width) ||
        !reader.u32(candidate.layout.depth) ||
        !valid_field_layout(candidate.layout))
        return fail(error, "hydrology field product layout is invalid");
    const auto count = static_cast<std::uint64_t>(candidate.layout.width) *
        candidate.layout.depth;
    const auto record_bytes = candidate.kind ==
            HydrologyFieldProductKind::Runtime
        ? kGameplayRecordBytes : kPresentationRecordBytes;
    if (count > (kMaxFieldPayloadBytes - kFieldLayoutBytes) / record_bytes ||
        reader.remaining != count * record_bytes)
        return fail(error, "hydrology field product record size is invalid");

    if (candidate.kind == HydrologyFieldProductKind::Runtime) {
        candidate.gameplay.resize(static_cast<std::size_t>(count));
        for (auto& sample : candidate.gameplay) {
            std::uint8_t wet = 0u;
            if (!reader.floating(sample.height_m) ||
                !reader.floating(sample.depth_m) ||
                !reader.floating(sample.velocity_x_mps) ||
                !reader.floating(sample.velocity_y_mps) ||
                !reader.floating(sample.velocity_z_mps) ||
                !reader.u8(wet) || wet > 1u)
                return fail(error, "runtime field sample is invalid");
            sample.wet_valid = wet != 0u;
        }
        candidate.payload_digest = hydrology_runtime_field_digest(
            candidate.layout, candidate.gameplay);
    } else {
        candidate.presentation.resize(static_cast<std::size_t>(count));
        for (auto& sample : candidate.presentation) {
            std::uint8_t feature = 0u;
            std::uint8_t wet = 0u;
            if (!reader.floating(sample.normal_x) ||
                !reader.floating(sample.normal_z) ||
                !reader.floating(sample.turbulence) ||
                !reader.floating(sample.aeration) ||
                !reader.floating(sample.foam_potential) ||
                !reader.u8(feature) ||
                feature > static_cast<std::uint8_t>(RiverFeature::Pool) ||
                !reader.u8(wet) || wet > 1u)
                return fail(error, "presentation field sample is invalid");
            sample.feature = static_cast<RiverFeature>(feature);
            sample.wet_valid = wet != 0u;
        }
        candidate.payload_digest = hydrology_presentation_field_digest(
            candidate.layout, candidate.presentation);
    }
    if (reader.remaining != 0u || candidate.payload_digest == 0u ||
        candidate.payload_digest != expected_digest)
        return fail(error, "hydrology field product digest is invalid");
    product = std::move(candidate);
    return true;
}

bool save_hydrology_field_product_atomic(
    const std::filesystem::path& path,
    const HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_hydrology_field_product(product, bytes, error)) return false;
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error)
            return fail(error, "could not create hydrology field directory");
    }
    static std::atomic<std::uint64_t> serial{0u};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++serial);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return fail(error, "could not create hydrology field temporary");
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return fail(error, "could not write hydrology field temporary");
        }
    }
    HydrologyFieldProduct reopened{};
    if (!load_hydrology_field_product_validated(
            temporary, product.kind, product.payload_digest,
            reopened, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    if (!replace_file(temporary, path)) {
        std::filesystem::remove(temporary, filesystem_error);
        return fail(error, "could not atomically publish hydrology field");
    }
    return true;
}

bool load_hydrology_field_product_validated(
    const std::filesystem::path& path,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    error = {};
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size < kFieldHeaderBytes ||
        size > kFieldHeaderBytes + kMaxFieldPayloadBytes)
        return fail(error, "hydrology field file size is invalid");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    HydrologyFieldProduct candidate{};
    if (!stream ||
        !deserialize_hydrology_field_product(bytes, candidate, error))
        return false;
    if (candidate.kind != expected_kind || expected_payload_digest == 0u ||
        candidate.payload_digest != expected_payload_digest)
        return fail(error, "hydrology field product is stale or type-mismatched");
    product = std::move(candidate);
    return true;
}

bool serialize_network_artifact(const HydrologyNetworkArtifact& artifact,
                                std::vector<std::uint8_t>& bytes,
                                gpu_meshing::Error& error) {
    bytes.clear();
    error = {};
    HydrologyNetworkArtifact canonical = artifact;
    canonicalize_references(canonical.sections);
    canonicalize_references(canonical.handoffs);
    canonicalize_field_products(canonical.field_products);
    if (!validate_manifest(canonical, false))
        return fail(error, "hydrology network manifest is invalid");

    Writer payload;
    payload.u8(static_cast<std::uint8_t>(canonical.state));
    payload.u64(canonical.network_key);
    payload.u64(canonical.terrain_revision);
    payload.u64(canonical.runtime_field_digest);
    payload.u64(canonical.presentation_field_digest);
    payload.u32(static_cast<std::uint32_t>(
        canonical.field_products.size()));
    for (const auto& product : canonical.field_products) {
        payload.u8(static_cast<std::uint8_t>(product.kind));
        payload.string(product.relative_path);
        payload.u64(product.payload_digest);
    }
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
        !reader.u64(candidate.runtime_field_digest) ||
        !reader.u64(candidate.presentation_field_digest) ||
        !reader.u32(count) || count > 2u)
        return fail(error, "hydrology network field closure is invalid");
    candidate.field_products.resize(count);
    for (auto& product : candidate.field_products) {
        std::uint8_t kind = 0u;
        if (!reader.u8(kind) ||
            kind > static_cast<std::uint8_t>(
                       HydrologyFieldProductKind::Presentation) ||
            !reader.string(product.relative_path) ||
            !reader.u64(product.payload_digest))
            return fail(error, "hydrology network field product is invalid");
        product.kind = static_cast<HydrologyFieldProductKind>(kind);
    }
    if (!reader.floating(candidate.bounds_m.minimum.x) ||
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
    canonicalize_field_products(candidate.field_products);
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
    if (!validate_field_package(path, artifact, error)) return false;
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
    HydrologyNetworkArtifact candidate{};
    if (!stream || !deserialize_network_artifact(bytes, candidate, error))
        return false;
    if (!validate_manifest(candidate, true) ||
        candidate.network_key != expected_network_key ||
        candidate.terrain_revision != expected_terrain_revision) {
        return fail(error, "hydrology network manifest is not ready or is stale");
    }
    if (!validate_field_package(path, candidate, error)) return false;
    artifact = std::move(candidate);
    return true;
}

} // namespace hydrology
