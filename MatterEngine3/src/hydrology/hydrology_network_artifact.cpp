#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "hydrology/hydrology_network_artifact.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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
std::atomic<HydrologyFieldValidationTestHook> g_field_validation_hook{nullptr};
std::atomic<void*> g_field_validation_context{nullptr};
std::atomic<HydrologyNamespaceValidationTestHook>
    g_namespace_validation_hook{nullptr};
std::atomic<void*> g_namespace_validation_context{nullptr};
std::atomic<HydrologyManifestPublicationTestHook>
    g_manifest_publication_hook{nullptr};
std::atomic<void*> g_manifest_publication_context{nullptr};
std::atomic<HydrologyFieldIoFailurePoint> g_field_io_failure{
    HydrologyFieldIoFailurePoint::None};

void inject_field_io_failure(HydrologyFieldIoFailurePoint point) {
    auto expected = point;
    if (g_field_io_failure.compare_exchange_strong(
            expected, HydrologyFieldIoFailurePoint::None,
            std::memory_order_acq_rel))
        throw std::bad_alloc();
}

void invoke_namespace_validation_hook(
    const std::filesystem::path& path) noexcept {
    if (const auto hook = g_namespace_validation_hook.load(
            std::memory_order_acquire))
        hook(path, g_namespace_validation_context.load(
                       std::memory_order_acquire));
}

void invoke_manifest_publication_hook(
    HydrologyManifestPublicationTestStage stage,
    const std::filesystem::path& path) noexcept {
    if (const auto hook = g_manifest_publication_hook.load(
            std::memory_order_acquire))
        hook(stage, path, g_manifest_publication_context.load(
                              std::memory_order_acquire));
}

bool fail(gpu_meshing::Error& error, const char* message) {
    error.code = gpu_meshing::ErrorCode::ArtifactFailure;
    error.message = message;
    return false;
}

bool fail_from_exception(gpu_meshing::Error& error,
                         const char* message) noexcept {
    error.code = gpu_meshing::ErrorCode::ArtifactFailure;
    try {
        error.message = message;
    } catch (...) {
        error.message.clear();
    }
    return false;
}

template <typename Operation>
bool translate_artifact_exceptions(gpu_meshing::Error& error,
                                   Operation operation) noexcept {
    try {
        return operation();
    } catch (const std::bad_alloc&) {
        return fail_from_exception(error,
                                   "hydrology artifact allocation failed");
    } catch (const std::filesystem::filesystem_error&) {
        return fail_from_exception(error,
                                   "hydrology artifact filesystem operation failed");
    } catch (const std::exception&) {
        return fail_from_exception(error,
                                   "hydrology artifact operation failed");
    } catch (...) {
        return fail_from_exception(error,
                                   "hydrology artifact operation failed");
    }
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

const char* field_kind_name(HydrologyFieldProductKind kind) {
    switch (kind) {
        case HydrologyFieldProductKind::Runtime: return "runtime";
        case HydrologyFieldProductKind::Presentation: return "presentation";
    }
    return nullptr;
}

bool exact_field_path(const HydrologyFieldProductReference& product) {
    return product.relative_path == hydrology_field_product_relative_path(
        product.kind, product.payload_digest);
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

bool load_confined_field_product(
    const std::filesystem::path& cache_root,
    const HydrologyFieldProductReference& reference,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error);

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
    if (!load_confined_field_product(cache_root, *runtime, runtime_product,
                                     error) ||
        !load_confined_field_product(cache_root, *presentation,
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
            !exact_field_path(product) ||
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

#ifdef _WIN32
class UniqueNativeHandle {
public:
    UniqueNativeHandle() noexcept = default;
    explicit UniqueNativeHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueNativeHandle() noexcept { reset(); }
    UniqueNativeHandle(const UniqueNativeHandle&) = delete;
    UniqueNativeHandle& operator=(const UniqueNativeHandle&) = delete;
    UniqueNativeHandle(UniqueNativeHandle&& other) noexcept
        : handle_(other.release()) {}
    UniqueNativeHandle& operator=(UniqueNativeHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }
    HANDLE release() noexcept {
        const HANDLE result = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(handle_);
        handle_ = handle;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#else
class UniqueNativeFd {
public:
    UniqueNativeFd() noexcept = default;
    explicit UniqueNativeFd(int descriptor) noexcept
        : descriptor_(descriptor) {}
    ~UniqueNativeFd() noexcept { reset(); }
    UniqueNativeFd(const UniqueNativeFd&) = delete;
    UniqueNativeFd& operator=(const UniqueNativeFd&) = delete;
    UniqueNativeFd(UniqueNativeFd&& other) noexcept
        : descriptor_(other.release()) {}
    UniqueNativeFd& operator=(UniqueNativeFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    int get() const noexcept { return descriptor_; }
    explicit operator bool() const noexcept { return descriptor_ >= 0; }
    int release() noexcept {
        const int result = descriptor_;
        descriptor_ = -1;
        return result;
    }
    void reset(int descriptor = -1) noexcept {
        if (*this) ::close(descriptor_);
        descriptor_ = descriptor;
    }
private:
    int descriptor_ = -1;
};
#endif

#ifndef _WIN32
bool read_posix_descriptor(int file,
                           const std::filesystem::path& path,
                           std::uint64_t minimum_size,
                           std::uint64_t maximum_size,
                           std::vector<std::uint8_t>& bytes,
                           struct stat& initial,
                           gpu_meshing::Error& error) {
    if (::fstat(file, &initial) != 0 || !S_ISREG(initial.st_mode) ||
        initial.st_size < 0 ||
        static_cast<std::uint64_t>(initial.st_size) < minimum_size ||
        static_cast<std::uint64_t>(initial.st_size) > maximum_size)
        return fail(error, "hydrology artifact file size or type is invalid");
    if (const auto hook = g_field_validation_hook.load(
            std::memory_order_acquire))
        hook(path, g_field_validation_context.load(std::memory_order_acquire));
    bytes.resize(static_cast<std::size_t>(initial.st_size));
    std::size_t offset = 0u;
    while (offset != bytes.size()) {
        const ssize_t read = ::read(file, bytes.data() + offset,
                                    bytes.size() - offset);
        if (read <= 0)
            return fail(error, "could not read hydrology artifact");
        offset += static_cast<std::size_t>(read);
    }
    std::uint8_t trailing = 0u;
    struct stat final{};
    if (::read(file, &trailing, 1u) != 0 || ::fstat(file, &final) != 0 ||
        initial.st_dev != final.st_dev || initial.st_ino != final.st_ino ||
        initial.st_size != final.st_size ||
        initial.st_mtim.tv_sec != final.st_mtim.tv_sec ||
        initial.st_mtim.tv_nsec != final.st_mtim.tv_nsec)
        return fail(error, "hydrology artifact changed while being read");
    return true;
}
#else
bool read_windows_handle(HANDLE file,
                         const std::filesystem::path& path,
                         std::uint64_t minimum_size,
                         std::uint64_t maximum_size,
                         std::vector<std::uint8_t>& bytes,
                         gpu_meshing::Error& error,
                         bool invoke_test_hook = true) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    const bool valid =
        GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) != 0 &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u &&
        GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0 &&
        static_cast<std::uint64_t>(size.QuadPart) >= minimum_size &&
        static_cast<std::uint64_t>(size.QuadPart) <= maximum_size;
    if (!valid)
        return fail(error, "hydrology artifact file size or type is invalid");
    if (invoke_test_hook) {
        if (const auto hook = g_field_validation_hook.load(
                std::memory_order_acquire))
            hook(path,
                 g_field_validation_context.load(std::memory_order_acquire));
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0u;
    while (offset != bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0u;
        if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) ||
            read == 0u)
            return fail(error, "could not read hydrology artifact");
        offset += read;
    }
    std::uint8_t trailing = 0u;
    DWORD trailing_read = 0u;
    LARGE_INTEGER final_size{};
    if (ReadFile(file, &trailing, 1u, &trailing_read, nullptr) == 0 ||
        trailing_read != 0u || GetFileSizeEx(file, &final_size) == 0 ||
        final_size.QuadPart != size.QuadPart)
        return fail(error, "hydrology artifact changed while being read");
    return true;
}
#endif

bool read_file_same_handle(const std::filesystem::path& path,
                           std::uint64_t minimum_size,
                           std::uint64_t maximum_size,
                           std::vector<std::uint8_t>& bytes,
                           gpu_meshing::Error& error) {
#ifdef _WIN32
    UniqueNativeHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file)
        return fail(error, "could not open hydrology artifact");
    inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterFileHandle);
    return read_windows_handle(file.get(), path, minimum_size, maximum_size,
                               bytes, error);
#else
    UniqueNativeFd file(
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!file) return fail(error, "could not open hydrology artifact");
    inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterFileHandle);
    struct stat initial{};
    const bool stable = read_posix_descriptor(
        file.get(), path, minimum_size, maximum_size, bytes, initial, error);
    return stable;
#endif
}

bool confined_field_components(const std::filesystem::path& cache_root,
                               const std::string& relative_path,
                               gpu_meshing::Error& error) {
    const std::filesystem::path relative(relative_path);
    if (!cache_relative_path(relative_path))
        return fail(error, "hydrology field path is not cache-relative");
    std::filesystem::path current = cache_root;
    for (const auto& part : relative) {
        current /= part;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u)
            return fail(error, "hydrology field path crosses a reparse point");
#else
        struct stat status{};
        if (::lstat(current.c_str(), &status) != 0 || S_ISLNK(status.st_mode))
            return fail(error, "hydrology field path crosses a symbolic link");
#endif
    }
    return true;
}

#ifdef _WIN32
class WindowsDirectoryGuard {
public:
    bool open(const std::filesystem::path& cache_root,
              const std::filesystem::path& relative_directory,
              gpu_meshing::Error& error,
              bool create_missing = false,
              bool allow_handle_relative_mutation = false) {
        share_mode_ = allow_handle_relative_mutation
            ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
            : FILE_SHARE_READ;
        handles_.reserve(3u);
        UniqueNativeHandle root(CreateFileW(
            cache_root.c_str(), GENERIC_READ, share_mode_, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!valid_directory(root.get()))
            return fail(error,
                        "hydrology cache root is not a trusted directory");
        inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterRootHandle);
        handles_.push_back(std::move(root));
        for (const auto& part : relative_directory) {
            const std::wstring name = part.native();
            UniqueNativeHandle child(open_relative_directory(
                handles_.back().get(), name, create_missing));
            if (!valid_directory(child.get()))
                return fail(error,
                            "hydrology field directory is not confined");
            inject_field_io_failure(
                HydrologyFieldIoFailurePoint::AfterDirectoryHandle);
            handles_.push_back(std::move(child));
        }
        return true;
    }

    HANDLE leaf() const noexcept {
        return handles_.empty() ? INVALID_HANDLE_VALUE : handles_.back().get();
    }

private:
    static bool valid_directory(HANDLE handle) noexcept {
        if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                          &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u)
            return false;
        return true;
    }

    HANDLE open_relative_directory(HANDLE root,
                                   const std::wstring& name,
                                   bool create_missing) const noexcept {
        using NtCreateFileFunction = NTSTATUS (NTAPI *)(
            PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        const auto create_file = reinterpret_cast<NtCreateFileFunction>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
        if (create_file == nullptr ||
            name.size() > std::numeric_limits<USHORT>::max() /
                              sizeof(wchar_t))
            return INVALID_HANDLE_VALUE;
        UNICODE_STRING unicode{};
        unicode.Buffer = const_cast<PWSTR>(name.data());
        unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
        unicode.MaximumLength = unicode.Length;
        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(&attributes, &unicode, OBJ_CASE_INSENSITIVE,
                                   root, nullptr);
        IO_STATUS_BLOCK status{};
        HANDLE result = INVALID_HANDLE_VALUE;
        const NTSTATUS native_status = create_file(
            &result, GENERIC_READ | SYNCHRONIZE, &attributes, &status, nullptr,
            FILE_ATTRIBUTE_DIRECTORY, share_mode_,
            create_missing ? FILE_OPEN_IF : FILE_OPEN,
            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                FILE_OPEN_REPARSE_POINT,
            nullptr, 0u);
        return native_status >= 0 ? result : INVALID_HANDLE_VALUE;
    }

    std::vector<UniqueNativeHandle> handles_;
    DWORD share_mode_ = FILE_SHARE_READ;
};
#else
class PosixDirectoryGuard {
public:
    bool open(const std::filesystem::path& cache_root,
              const std::filesystem::path& relative_directory,
              gpu_meshing::Error& error,
              bool create_missing = false) {
        descriptors_.reserve(3u);
        UniqueNativeFd root(::open(cache_root.c_str(), O_RDONLY | O_DIRECTORY |
                                   O_CLOEXEC | O_NOFOLLOW));
        if (!root)
            return fail(error,
                        "hydrology cache root is not a trusted directory");
        inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterRootHandle);
        descriptors_.push_back(std::move(root));
        for (const auto& part : relative_directory) {
            const auto name = part.native();
            int child = ::openat(descriptors_.back().get(), name.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                     O_NOFOLLOW);
            if (child < 0 && create_missing && errno == ENOENT &&
                ::mkdirat(descriptors_.back().get(), name.c_str(), 0700) == 0)
                child = ::openat(descriptors_.back().get(), name.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                     O_NOFOLLOW);
            UniqueNativeFd owned_child(child);
            if (!owned_child)
                return fail(error,
                            "hydrology field directory is not confined");
            inject_field_io_failure(
                HydrologyFieldIoFailurePoint::AfterDirectoryHandle);
            descriptors_.push_back(std::move(owned_child));
        }
        return true;
    }

    int leaf() const noexcept {
        return descriptors_.empty() ? -1 : descriptors_.back().get();
    }

private:
    std::vector<UniqueNativeFd> descriptors_;
};

#endif

bool decode_field_product_bytes(
    const std::vector<std::uint8_t>& bytes,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    HydrologyFieldProduct candidate{};
    if (!deserialize_hydrology_field_product(bytes, candidate, error))
        return false;
    if (candidate.kind != expected_kind || expected_payload_digest == 0u ||
        candidate.payload_digest != expected_payload_digest)
        return fail(error,
                    "hydrology field product is stale or type-mismatched");
    product = std::move(candidate);
    return true;
}

#ifdef _WIN32
HANDLE open_relative_file(HANDLE root,
                          const std::wstring& name,
                          ACCESS_MASK desired_access,
                          ULONG share_access,
                          ULONG disposition,
                          ULONG options) noexcept;

bool load_windows_relative_field(
    HANDLE directory,
    const std::filesystem::path& leaf,
    const std::filesystem::path& diagnostic_path,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    const std::wstring leaf_name = leaf.native();
    UniqueNativeHandle field(open_relative_file(
        directory, leaf_name, GENERIC_READ | SYNCHRONIZE, FILE_SHARE_READ,
        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT));
    if (!field) return fail(error, "could not open confined hydrology field");
    inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterFileHandle);
    std::vector<std::uint8_t> bytes;
    if (!read_windows_handle(
            field.get(), diagnostic_path, kFieldHeaderBytes,
            kFieldHeaderBytes + kMaxFieldPayloadBytes, bytes, error))
        return false;
    return decode_field_product_bytes(
        bytes, expected_kind, expected_payload_digest, product, error);
}
#else
HydrologyFileIdentity native_file_identity(const struct stat& status) noexcept {
    return {static_cast<std::uint64_t>(status.st_dev),
            static_cast<std::uint64_t>(status.st_ino)};
}

bool posix_named_directory_matches(
    const std::filesystem::path& path,
    int held) {
    UniqueNativeFd named(::open(
        path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat held_status{};
    struct stat named_status{};
    return named && ::fstat(held, &held_status) == 0 &&
           ::fstat(named.get(), &named_status) == 0 &&
           hydrology_file_identity_stable(
               native_file_identity(held_status),
               native_file_identity(named_status),
               native_file_identity(named_status));
}

bool read_posix_relative_bytes_stable(
    int directory,
    const std::filesystem::path& leaf,
    const std::filesystem::path& diagnostic_path,
    std::uint64_t minimum_size,
    std::uint64_t maximum_size,
    std::vector<std::uint8_t>& bytes,
    struct stat& opened,
    gpu_meshing::Error& error) {
    UniqueNativeFd field(::openat(
        directory, leaf.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!field) return fail(error, "could not open confined hydrology field");
    inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterFileHandle);
    struct stat descriptor_before{};
    struct stat named_before{};
    if (::fstat(field.get(), &descriptor_before) != 0 ||
        ::fstatat(directory, leaf.c_str(), &named_before,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named_before.st_mode) ||
        !hydrology_file_identity_stable(
            native_file_identity(descriptor_before),
            native_file_identity(named_before),
            native_file_identity(named_before)))
        return fail(error, "hydrology field identity is invalid");
    if (!read_posix_descriptor(
            field.get(), diagnostic_path, minimum_size, maximum_size, bytes,
            opened, error))
        return false;
    struct stat named_after{};
    if (::fstatat(directory, leaf.c_str(), &named_after,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named_after.st_mode) ||
        !hydrology_file_identity_stable(
            native_file_identity(opened), native_file_identity(named_before),
            native_file_identity(named_after)))
        return fail(error,
                    "hydrology field identity changed during validation");
    return true;
}

bool load_posix_relative_field(
    int directory,
    const std::filesystem::path& leaf,
    const std::filesystem::path& diagnostic_path,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    struct stat opened{};
    if (!read_posix_relative_bytes_stable(
            directory, leaf, diagnostic_path, kFieldHeaderBytes,
            kFieldHeaderBytes + kMaxFieldPayloadBytes, bytes, opened, error))
        return false;
    return decode_field_product_bytes(
        bytes, expected_kind, expected_payload_digest, product, error);
}
#endif

bool load_confined_field_product(
    const std::filesystem::path& cache_root,
    const HydrologyFieldProductReference& reference,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    if (!exact_field_path(reference) ||
        !confined_field_components(cache_root, reference.relative_path, error))
        return false;
#ifdef _WIN32
    const std::filesystem::path relative(reference.relative_path);
    WindowsDirectoryGuard directories;
    if (!directories.open(cache_root, relative.parent_path(), error))
        return false;
    invoke_namespace_validation_hook(cache_root / relative);
    return load_windows_relative_field(
        directories.leaf(), relative.filename(), cache_root / relative,
        reference.kind, reference.payload_digest, product, error);
#else
    const std::filesystem::path relative(reference.relative_path);
    PosixDirectoryGuard directories;
    if (!directories.open(cache_root, relative.parent_path(), error))
        return false;
    invoke_namespace_validation_hook(cache_root / relative);
    HydrologyFieldProduct candidate{};
    const bool loaded = load_posix_relative_field(
        directories.leaf(), relative.filename(), cache_root / relative,
        reference.kind, reference.payload_digest, candidate, error);
    if (!loaded ||
        !confined_field_components(cache_root, reference.relative_path, error))
        return false;
    product = std::move(candidate);
    return true;
#endif
}

#ifdef _WIN32
HANDLE open_relative_file(HANDLE root,
                          const std::wstring& name,
                          ACCESS_MASK desired_access,
                          ULONG share_access,
                          ULONG disposition,
                          ULONG options) noexcept {
    using NtCreateFileFunction = NTSTATUS (NTAPI *)(
        PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    const auto create_file = reinterpret_cast<NtCreateFileFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    if (create_file == nullptr ||
        name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
        return INVALID_HANDLE_VALUE;
    UNICODE_STRING unicode{};
    unicode.Buffer = const_cast<PWSTR>(name.data());
    unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode.MaximumLength = unicode.Length;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &unicode, OBJ_CASE_INSENSITIVE,
                               root, nullptr);
    IO_STATUS_BLOCK status{};
    HANDLE result = INVALID_HANDLE_VALUE;
    const NTSTATUS native_status = create_file(
        &result, desired_access, &attributes, &status, nullptr,
        FILE_ATTRIBUTE_NORMAL, share_access, disposition,
        options | FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, nullptr,
        0u);
    return native_status >= 0 ? result : INVALID_HANDLE_VALUE;
}

void mark_file_delete(HANDLE handle) noexcept {
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                               sizeof(disposition));
}

class WindowsTemporaryFile {
public:
    explicit WindowsTemporaryFile(HANDLE handle) noexcept : handle_(handle) {}
    ~WindowsTemporaryFile() noexcept {
        if (armed_ && handle_) mark_file_delete(handle_.get());
    }
    WindowsTemporaryFile(const WindowsTemporaryFile&) = delete;
    WindowsTemporaryFile& operator=(const WindowsTemporaryFile&) = delete;
    HANDLE get() const noexcept { return handle_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(handle_); }
    void disarm() noexcept { armed_ = false; }
    void disarm_and_close() noexcept {
        armed_ = false;
        handle_.reset();
    }
private:
    UniqueNativeHandle handle_;
    bool armed_ = true;
};

bool publish_file_create_new_relative(
    HANDLE source_handle,
    const std::wstring& target_name,
    HANDLE trusted_directory) {
    struct NativeIoStatusBlock {
        union { long status; void* pointer; } value;
        std::uintptr_t information;
    };
    struct NativeFileRenameInformation {
        BOOLEAN replace_if_exists;
        HANDLE root_directory;
        ULONG file_name_length;
        WCHAR file_name[1];
    };
    using NtSetInformationFile = long (NTAPI *)(
        HANDLE, NativeIoStatusBlock*, void*, ULONG, int);
    const auto set_information = reinterpret_cast<NtSetInformationFile>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtSetInformationFile"));
    if (set_information == nullptr) {
        return false;
    }
    const std::size_t bytes =
        offsetof(NativeFileRenameInformation, file_name) +
        target_name.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> storage(bytes, 0u);
    auto* const rename = reinterpret_cast<NativeFileRenameInformation*>(
        storage.data());
    rename->replace_if_exists = FALSE;
    rename->root_directory = trusted_directory;
    rename->file_name_length =
        static_cast<DWORD>(target_name.size() * sizeof(wchar_t));
    std::memcpy(rename->file_name, target_name.data(),
                rename->file_name_length);
    NativeIoStatusBlock status{};
    constexpr int kFileRenameInformation = 10;
    const long native_status = set_information(
        source_handle, &status, rename, static_cast<ULONG>(storage.size()),
        kFileRenameInformation);
    const bool published = native_status >= 0;
    return published;
}

bool read_windows_relative_bytes_exact(
    HANDLE directory,
    const std::wstring& leaf,
    const std::filesystem::path& diagnostic_path,
    const std::vector<std::uint8_t>& expected,
    gpu_meshing::Error& error) {
    UniqueNativeHandle existing(open_relative_file(
        directory, leaf, GENERIC_READ | SYNCHRONIZE, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT));
    if (!existing)
        return fail(error, "could not open existing immutable hydrology manifest");
    std::vector<std::uint8_t> existing_bytes;
    if (!read_windows_handle(
            existing.get(), diagnostic_path, kHeaderBytes,
            kHeaderBytes + kMaxPayloadBytes, existing_bytes, error, false))
        return false;
    if (existing_bytes != expected)
        return fail(error,
                    "immutable hydrology manifest already has different bytes");
    return true;
}

bool same_windows_file_identity(HANDLE left, HANDLE right) noexcept {
    BY_HANDLE_FILE_INFORMATION left_info{};
    BY_HANDLE_FILE_INFORMATION right_info{};
    return GetFileInformationByHandle(left, &left_info) != 0 &&
           GetFileInformationByHandle(right, &right_info) != 0 &&
           left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
           left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
           left_info.nFileIndexLow == right_info.nFileIndexLow;
}

bool windows_named_directory_matches(
    const std::filesystem::path& path,
    HANDLE held) {
    UniqueNativeHandle named(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    return named && GetFileInformationByHandleEx(
               named.get(), FileAttributeTagInfo, &attributes,
               sizeof(attributes)) != 0 &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
           same_windows_file_identity(held, named.get());
}
#endif

} // namespace

std::string hydrology_field_product_relative_path(
    HydrologyFieldProductKind kind,
    std::uint64_t payload_digest) {
    const char* const name = field_kind_name(kind);
    if (name == nullptr || payload_digest == 0u) return {};
    constexpr char digits[] = "0123456789abcdef";
    char encoded[16];
    for (std::size_t index = 0u; index != sizeof(encoded); ++index) {
        const unsigned shift = static_cast<unsigned>((15u - index) * 4u);
        encoded[index] = digits[(payload_digest >> shift) & 0x0fu];
    }
    std::string path = "hydrology/fields/";
    path += name;
    path += '-';
    path.append(encoded, sizeof(encoded));
    path += ".mhydfield";
    return path;
}

void set_hydrology_field_validation_test_hook(
    HydrologyFieldValidationTestHook hook,
    void* context) noexcept {
    g_field_validation_context.store(context, std::memory_order_release);
    g_field_validation_hook.store(hook, std::memory_order_release);
}

void set_hydrology_field_io_failure_for_test(
    HydrologyFieldIoFailurePoint point) noexcept {
    g_field_io_failure.store(point, std::memory_order_release);
}

void set_hydrology_namespace_validation_test_hook(
    HydrologyNamespaceValidationTestHook hook,
    void* context) noexcept {
    g_namespace_validation_context.store(context, std::memory_order_release);
    g_namespace_validation_hook.store(hook, std::memory_order_release);
}

void set_hydrology_manifest_publication_test_hook(
    HydrologyManifestPublicationTestHook hook,
    void* context) noexcept {
    g_manifest_publication_context.store(context, std::memory_order_release);
    g_manifest_publication_hook.store(hook, std::memory_order_release);
}

bool hydrology_file_identity_stable(
    HydrologyFileIdentity opened,
    HydrologyFileIdentity named_before,
    HydrologyFileIdentity named_after) noexcept {
    return opened.device == named_before.device &&
           opened.file == named_before.file &&
           opened.device == named_after.device &&
           opened.file == named_after.file;
}

bool format_hydrology_proc_fd_path(
    int descriptor,
    char* buffer,
    std::size_t capacity) noexcept {
    if (buffer == nullptr || capacity == 0u) return false;
    buffer[0] = '\0';
    if (descriptor < 0) return false;
    constexpr char prefix[] = "/proc/self/fd/";
    constexpr std::size_t prefix_size = sizeof(prefix) - 1u;
    if (capacity <= prefix_size + 1u) return false;
    std::memcpy(buffer, prefix, prefix_size);
    const auto converted = std::to_chars(
        buffer + prefix_size, buffer + capacity - 1u, descriptor);
    if (converted.ec != std::errc{}) {
        buffer[0] = '\0';
        return false;
    }
    *converted.ptr = '\0';
    return true;
}

#ifndef _WIN32
enum class PosixRetainedFdPublicationResult : std::uint8_t {
    Published,
    Exists,
    Failed,
};

static PosixRetainedFdPublicationResult
publish_posix_retained_fd_create_new(
    int source_descriptor,
    HydrologyFileIdentity validated_source,
    int destination_directory,
    const char* canonical_name) noexcept {
    char source_path[64]{};
    if (canonical_name == nullptr || canonical_name[0] == '\0' ||
        !format_hydrology_proc_fd_path(
            source_descriptor, source_path, sizeof(source_path))) {
        errno = source_descriptor < 0 ? EBADF : ENAMETOOLONG;
        return PosixRetainedFdPublicationResult::Failed;
    }
    if (::linkat(AT_FDCWD, source_path, destination_directory,
                 canonical_name, AT_SYMLINK_FOLLOW) != 0) {
        const int link_error = errno;
        errno = link_error;
        return link_error == EEXIST
            ? PosixRetainedFdPublicationResult::Exists
            : PosixRetainedFdPublicationResult::Failed;
    }
    struct stat source_status{};
    struct stat destination_status{};
    int publication_error = 0;
    if (::fstat(source_descriptor, &source_status) != 0)
        publication_error = errno;
    else if (::fstatat(destination_directory, canonical_name,
                       &destination_status, AT_SYMLINK_NOFOLLOW) != 0)
        publication_error = errno;
    else if (!S_ISREG(source_status.st_mode) ||
             !S_ISREG(destination_status.st_mode) ||
             !hydrology_file_identity_stable(
                 validated_source, native_file_identity(source_status),
                 native_file_identity(destination_status)))
        publication_error = ESTALE;
    else if (::fsync(destination_directory) != 0)
        publication_error = errno;
    if (publication_error != 0) {
        errno = publication_error;
        return PosixRetainedFdPublicationResult::Failed;
    }
    return PosixRetainedFdPublicationResult::Published;
}
#endif

static bool serialize_hydrology_field_product_impl(
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

bool serialize_hydrology_field_product(
    const HydrologyFieldProduct& product,
    std::vector<std::uint8_t>& bytes,
    gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return serialize_hydrology_field_product_impl(product, bytes, error);
    });
}

static bool deserialize_hydrology_field_product_impl(
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

bool deserialize_hydrology_field_product(
    const std::vector<std::uint8_t>& bytes,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return deserialize_hydrology_field_product_impl(bytes, product, error);
    });
}

static bool save_hydrology_field_product_atomic_impl(
    const std::filesystem::path& path,
    const HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_hydrology_field_product(product, bytes, error)) return false;
    const auto canonical = hydrology_field_product_relative_path(
        product.kind, product.payload_digest);
    const std::filesystem::path canonical_path(canonical);
    if (canonical.empty() || path.filename() != canonical_path.filename() ||
        path.parent_path().filename() != "fields" ||
        path.parent_path().parent_path().filename() != "hydrology")
        return fail(error, "hydrology field target is not canonical");
    const auto cache_root = path.parent_path().parent_path().parent_path();
#ifdef _WIN32
    WindowsDirectoryGuard directory_guard;
    if (!directory_guard.open(cache_root, canonical_path.parent_path(), error,
                              true, true))
        return false;
#else
    PosixDirectoryGuard directory_guard;
    if (!directory_guard.open(cache_root, canonical_path.parent_path(), error,
                              true))
        return false;
#endif
#ifndef _WIN32
    const auto target_name = path.filename();
    const int directory = directory_guard.leaf();
#if defined(O_TMPFILE) && defined(AT_SYMLINK_FOLLOW)
    UniqueNativeFd temporary_file(::openat(
        directory, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600));
    if (!temporary_file)
        return fail(error, "could not create hydrology field temporary");
    inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterFileHandle);
    std::size_t offset = 0u;
    bool written = true;
    while (offset != bytes.size()) {
        const ssize_t count = ::write(temporary_file.get(), bytes.data() + offset,
                                      bytes.size() - offset);
        if (count <= 0) { written = false; break; }
        offset += static_cast<std::size_t>(count);
    }
    if (written) written = ::fsync(temporary_file.get()) == 0 &&
                           ::lseek(temporary_file.get(), 0, SEEK_SET) == 0;
    std::vector<std::uint8_t> reopened;
    struct stat reopened_status{};
    if (!written || !read_posix_descriptor(
            temporary_file.get(), path, kFieldHeaderBytes,
            kFieldHeaderBytes + kMaxFieldPayloadBytes, reopened,
            reopened_status, error) || reopened != bytes) {
        return fail(error, "hydrology field temporary validation failed");
    }
    const auto published = publish_posix_retained_fd_create_new(
        temporary_file.get(), native_file_identity(reopened_status), directory,
        target_name.c_str());
    if (published != PosixRetainedFdPublicationResult::Published) {
        if (published != PosixRetainedFdPublicationResult::Exists)
            return fail(error, "could not publish hydrology field product");
        std::vector<std::uint8_t> existing;
        struct stat existing_status{};
        const bool exact = read_posix_relative_bytes_stable(
            directory, target_name, path, kFieldHeaderBytes,
            kFieldHeaderBytes + kMaxFieldPayloadBytes, existing,
            existing_status, error) && existing == bytes;
        if (!exact)
            return fail(error,
                        "immutable hydrology field already has different bytes");
        return true;
    }
    return true;
#else
    (void)target_name;
    (void)directory;
    return fail(error,
                "secure POSIX hydrology field publication is unavailable");
#endif
#else
    static std::atomic<std::uint64_t> serial{0u};
    const std::filesystem::path temporary_name =
        path.filename().string() + ".tmp-" + std::to_string(++serial);
    const std::wstring temporary_native = temporary_name.native();
    const std::wstring target_native = path.filename().native();
    WindowsTemporaryFile temporary(open_relative_file(
        directory_guard.leaf(), temporary_native,
        GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ, FILE_CREATE,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH));
    if (!temporary)
        return fail(error, "could not create confined hydrology temporary");
    inject_field_io_failure(HydrologyFieldIoFailurePoint::AfterFileHandle);
    std::size_t offset = 0u;
    while (offset != bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0u;
        if (!WriteFile(temporary.get(), bytes.data() + offset, request,
                       &written, nullptr) || written == 0u)
            return fail(error, "could not write confined hydrology temporary");
        offset += written;
    }
    LARGE_INTEGER beginning{};
    if (FlushFileBuffers(temporary.get()) == 0 ||
        SetFilePointerEx(temporary.get(), beginning, nullptr, FILE_BEGIN) == 0)
        return fail(error, "could not flush confined hydrology temporary");
    std::vector<std::uint8_t> reopened;
    if (!read_windows_handle(
            temporary.get(), temporary_name, kFieldHeaderBytes,
            kFieldHeaderBytes + kMaxFieldPayloadBytes, reopened, error,
            false) || reopened != bytes) {
        return fail(error, "hydrology field temporary validation failed");
    }
    const bool published = publish_file_create_new_relative(
        temporary.get(), target_native, directory_guard.leaf());
    if (!published) {
        mark_file_delete(temporary.get());
        UniqueNativeHandle existing_handle(open_relative_file(
            directory_guard.leaf(), target_native, GENERIC_READ | SYNCHRONIZE,
            FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT));
        std::vector<std::uint8_t> existing_bytes;
        if (!existing_handle || !read_windows_handle(
                existing_handle.get(), path.filename(), kFieldHeaderBytes,
                kFieldHeaderBytes + kMaxFieldPayloadBytes, existing_bytes,
                error, false) || existing_bytes != bytes)
            return fail(error,
                        "immutable hydrology field already has different bytes");
        return true;
    }
    temporary.disarm();
    return true;
#endif
}

bool save_hydrology_field_product_atomic(
    const std::filesystem::path& path,
    const HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return save_hydrology_field_product_atomic_impl(path, product, error);
    });
}

static bool load_hydrology_field_product_validated_impl(
    const std::filesystem::path& path,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    error = {};
    const std::filesystem::path parent = path.parent_path().empty()
        ? std::filesystem::path(".") : path.parent_path();
    const std::filesystem::path leaf = path.filename();
    if (leaf.empty()) return fail(error, "hydrology field path is invalid");
#ifdef _WIN32
    WindowsDirectoryGuard directory;
    if (!directory.open(parent, {}, error)) return false;
    invoke_namespace_validation_hook(path);
    return load_windows_relative_field(
        directory.leaf(), leaf, path, expected_kind, expected_payload_digest,
        product, error);
#else
    PosixDirectoryGuard directory;
    if (!directory.open(parent, {}, error)) return false;
    invoke_namespace_validation_hook(path);
    HydrologyFieldProduct candidate{};
    if (!load_posix_relative_field(
            directory.leaf(), leaf, path, expected_kind,
            expected_payload_digest, candidate, error))
        return false;
    if (!posix_named_directory_matches(parent, directory.leaf()))
        return fail(error, "hydrology field parent identity changed");
    product = std::move(candidate);
    return true;
#endif
}

bool load_hydrology_field_product_validated(
    const std::filesystem::path& path,
    HydrologyFieldProductKind expected_kind,
    std::uint64_t expected_payload_digest,
    HydrologyFieldProduct& product,
    gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return load_hydrology_field_product_validated_impl(
            path, expected_kind, expected_payload_digest, product, error);
    });
}

static bool serialize_network_artifact_impl(
    const HydrologyNetworkArtifact& artifact,
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

bool serialize_network_artifact(const HydrologyNetworkArtifact& artifact,
                                std::vector<std::uint8_t>& bytes,
                                gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return serialize_network_artifact_impl(artifact, bytes, error);
    });
}

static bool deserialize_network_artifact_impl(
    const std::vector<std::uint8_t>& bytes,
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

bool deserialize_network_artifact(const std::vector<std::uint8_t>& bytes,
                                  HydrologyNetworkArtifact& artifact,
                                  gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return deserialize_network_artifact_impl(bytes, artifact, error);
    });
}

static bool save_network_artifact_atomic_impl(
    const std::filesystem::path& path,
    const HydrologyNetworkArtifact& artifact,
    gpu_meshing::Error& error) {
    std::vector<std::uint8_t> bytes;
    if (!serialize_network_artifact(artifact, bytes, error)) return false;
    if (!validate_field_package(path, artifact, error)) return false;
    const std::filesystem::path parent = path.parent_path().empty()
        ? std::filesystem::path(".") : path.parent_path();
    const std::filesystem::path target_name = path.filename();
    if (target_name.empty())
        return fail(error, "hydrology manifest target is invalid");
#ifdef _WIN32
    static std::atomic<std::uint64_t> serial{0u};
    const std::filesystem::path temporary_name =
        target_name.string() + ".tmp-" + std::to_string(++serial);
    const std::filesystem::path temporary_path = parent / temporary_name;
    WindowsDirectoryGuard parent_guard;
    if (!parent_guard.open(parent, {}, error, false, true)) return false;
    inject_field_io_failure(
        HydrologyFieldIoFailurePoint::AfterManifestParentHandle);
    invoke_namespace_validation_hook(path);
    if (!windows_named_directory_matches(parent, parent_guard.leaf()))
        return fail(error, "hydrology manifest parent identity changed");
    const std::wstring target_native = target_name.native();
    const std::wstring temporary_native = temporary_name.native();
    invoke_namespace_validation_hook(temporary_path);
    WindowsTemporaryFile temporary(open_relative_file(
        parent_guard.leaf(), temporary_native,
        GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ, FILE_CREATE,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH));
    if (!temporary)
        return fail(error, "could not create confined hydrology manifest temporary");
    inject_field_io_failure(
        HydrologyFieldIoFailurePoint::AfterManifestFileHandle);
    std::size_t offset = 0u;
    while (offset != bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0u;
        if (!WriteFile(temporary.get(), bytes.data() + offset, request,
                       &written, nullptr) || written == 0u)
            return fail(error, "could not write hydrology manifest temporary");
        offset += written;
    }
    LARGE_INTEGER beginning{};
    if (FlushFileBuffers(temporary.get()) == 0 ||
        SetFilePointerEx(temporary.get(), beginning, nullptr, FILE_BEGIN) == 0)
        return fail(error, "could not flush hydrology manifest temporary");
    std::vector<std::uint8_t> reopened;
    if (!read_windows_handle(
            temporary.get(), path, kHeaderBytes,
            kHeaderBytes + kMaxPayloadBytes, reopened, error, false) ||
        reopened != bytes)
        return fail(error, "could not validate hydrology manifest temporary");
    HydrologyNetworkArtifact validated{};
    if (!deserialize_network_artifact(reopened, validated, error)) return false;
    invoke_manifest_publication_hook(
        HydrologyManifestPublicationTestStage::BeforeCommit, temporary_path);
    inject_field_io_failure(HydrologyFieldIoFailurePoint::BeforeManifestRename);
    if (!windows_named_directory_matches(parent, parent_guard.leaf()))
        return fail(error, "hydrology manifest parent identity changed");
    if (publish_file_create_new_relative(
            temporary.get(), target_native, parent_guard.leaf())) {
        temporary.disarm_and_close();
        invoke_manifest_publication_hook(
            HydrologyManifestPublicationTestStage::AfterCommit, path);
        return true;
    }
    if (!read_windows_relative_bytes_exact(
            parent_guard.leaf(), target_native, path, bytes, error) ||
        !windows_named_directory_matches(parent, parent_guard.leaf()))
        return false;
    return true;
#else
    PosixDirectoryGuard parent_guard;
    if (!parent_guard.open(parent, {}, error)) return false;
    inject_field_io_failure(
        HydrologyFieldIoFailurePoint::AfterManifestParentHandle);
    invoke_namespace_validation_hook(path);
    if (!posix_named_directory_matches(parent, parent_guard.leaf()))
        return fail(error, "hydrology manifest parent identity changed");
    const int directory = parent_guard.leaf();
#if defined(O_TMPFILE) && defined(AT_SYMLINK_FOLLOW)
    UniqueNativeFd temporary(::openat(
        directory, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600));
    if (!temporary)
        return fail(error, "could not create hydrology manifest temporary");
    inject_field_io_failure(
        HydrologyFieldIoFailurePoint::AfterManifestFileHandle);
    std::size_t offset = 0u;
    while (offset != bytes.size()) {
        const ssize_t written = ::write(
            temporary.get(), bytes.data() + offset, bytes.size() - offset);
        if (written <= 0)
            return fail(error, "could not write hydrology manifest temporary");
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(temporary.get()) != 0 ||
        ::lseek(temporary.get(), 0, SEEK_SET) != 0)
        return fail(error, "could not flush hydrology manifest temporary");
    std::vector<std::uint8_t> reopened;
    struct stat reopened_status{};
    if (!read_posix_descriptor(
            temporary.get(), path, kHeaderBytes,
            kHeaderBytes + kMaxPayloadBytes, reopened, reopened_status,
            error) || reopened != bytes)
        return fail(error, "could not validate hydrology manifest temporary");
    HydrologyNetworkArtifact validated{};
    if (!deserialize_network_artifact(reopened, validated, error)) return false;
    invoke_manifest_publication_hook(
        HydrologyManifestPublicationTestStage::BeforeCommit, path);
    inject_field_io_failure(HydrologyFieldIoFailurePoint::BeforeManifestRename);
    if (!posix_named_directory_matches(parent, parent_guard.leaf()))
        return fail(error, "hydrology manifest parent identity changed");
    const auto published = publish_posix_retained_fd_create_new(
        temporary.get(), native_file_identity(reopened_status), directory,
        target_name.c_str());
    if (published != PosixRetainedFdPublicationResult::Published) {
        if (published != PosixRetainedFdPublicationResult::Exists)
            return fail(error, "could not publish hydrology network manifest");
        std::vector<std::uint8_t> existing;
        struct stat existing_status{};
        if (!read_posix_relative_bytes_stable(
                directory, target_name, path, kHeaderBytes,
                kHeaderBytes + kMaxPayloadBytes, existing, existing_status,
                error) || existing != bytes)
            return fail(
                error,
                "immutable hydrology manifest already has different bytes");
        if (!posix_named_directory_matches(parent, parent_guard.leaf()))
            return fail(error, "hydrology manifest parent identity changed");
        return true;
    }
    invoke_manifest_publication_hook(
        HydrologyManifestPublicationTestStage::AfterCommit, path);
    return true;
#else
    (void)directory;
    return fail(error,
                "secure POSIX hydrology manifest publication is unavailable");
#endif
#endif
}

bool save_network_artifact_atomic(const std::filesystem::path& path,
                                  const HydrologyNetworkArtifact& artifact,
                                  gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return save_network_artifact_atomic_impl(path, artifact, error);
    });
}

static bool load_network_artifact_validated_impl(
    const std::filesystem::path& path,
    std::uint64_t expected_network_key,
    std::uint64_t expected_terrain_revision,
    HydrologyNetworkArtifact& artifact,
    gpu_meshing::Error& error) {
    error = {};
    std::vector<std::uint8_t> bytes;
    if (!read_file_same_handle(path, kHeaderBytes,
                               kHeaderBytes + kMaxPayloadBytes,
                               bytes, error))
        return false;
    HydrologyNetworkArtifact candidate{};
    if (!deserialize_network_artifact(bytes, candidate, error))
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

bool load_network_artifact_validated(
    const std::filesystem::path& path,
    std::uint64_t expected_network_key,
    std::uint64_t expected_terrain_revision,
    HydrologyNetworkArtifact& artifact,
    gpu_meshing::Error& error) {
    return translate_artifact_exceptions(error, [&] {
        return load_network_artifact_validated_impl(
            path, expected_network_key, expected_terrain_revision, artifact,
            error);
    });
}

} // namespace hydrology
