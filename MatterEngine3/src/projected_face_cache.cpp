#include "projected_face_cache.h"
#include "part_asset_v2.h"
#include "part_asset.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>

namespace gpu_meshing {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kMagic = 0x43414650; // PFAC
constexpr std::uint32_t kVersion = 1, kHeaderBytes = 160, kRecordBytes = 20;
constexpr std::size_t kChecksumOffset = 152;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "finite face cache requires IEEE binary32");
bool fail(Error &e, ErrorCode code, const char *message) { e = {code, message}; return false; }
bool current(const FaceJob &j, const BuildControl &c, Error &e) {
    if (c.cancelled && c.cancelled()) return fail(e, ErrorCode::Cancelled, "face cache cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.source.generation))
        return fail(e, ErrorCode::StaleGeneration, "face cache generation stale");
    return true;
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void u32(Bytes &b, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(std::uint8_t(n >> (8 * i)));
}
void u64(Bytes &b, std::uint64_t n) { u32(b, std::uint32_t(n)); u32(b, std::uint32_t(n >> 32)); }
void f32(Bytes &b, float f) { std::uint32_t n; std::memcpy(&n, &f, 4); u32(b, n); }
std::uint32_t read32(const std::uint8_t *b) {
    return std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 |
           std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24;
}
std::uint64_t read64(const std::uint8_t *b) { return read32(b) | std::uint64_t(read32(b + 4)) << 32; }
float read_float(const std::uint8_t *b) { const auto n = read32(b); float f; std::memcpy(&f, &n, 4); return f; }
std::uint64_t checksum(Bytes &b) {
    std::uint8_t stored[8];
    std::memcpy(stored, b.data() + kChecksumOffset, 8);
    std::memset(b.data() + kChecksumOffset, 0, 8);
    const auto h = part_asset::fnv1a64(b.data(), b.size());
    std::memcpy(b.data() + kChecksumOffset, stored, 8);
    return h;
}
Bytes header(const FacePatch &p) {
    Bytes b; b.reserve(kHeaderBytes);
    u32(b, kMagic); u32(b, kVersion); u32(b, kHeaderBytes); u32(b, solid_face_projection_version);
    u32(b, p.layout.width); u32(b, p.layout.height);
    for (float f : {p.u_min_m, p.u_max_m, p.v_min_m, p.v_max_m, p.height_min_m, p.height_max_m,
                    p.layout.pitch_u_m, p.layout.pitch_v_m}) f32(b, f);
    for (auto v : {p.frame.origin_m, p.frame.u, p.frame.v, p.frame.n,
                   p.layout.source_bounds.min_m, p.layout.source_bounds.max_m}) {
        f32(b, v.x); f32(b, v.y); f32(b, v.z);
    }
    u32(b, p.material); u32(b, 0); u64(b, p.recipe_digest);
    u64(b, std::uint64_t(p.layout.width) * p.layout.height * kRecordBytes);
    u64(b, 0);
    return b;
}
bool expected(const FaceJob &j, FacePatch &p, Error &e) {
    FaceLayout l; std::vector<FaceRegion> regions;
    if (!plan_face_regions(j, {}, l, regions, e)) return false;
    p = face_patch_metadata(j, l);
    return true;
}
bool valid_texel(const FaceTexel &t, const FaceJob &j) {
    const auto n = t.normal_uvn;
    if (t.coverage > 1 || !std::isfinite(t.height_m) || !std::isfinite(n.x) ||
        !std::isfinite(n.y) || !std::isfinite(n.z)) return false;
    if (!t.coverage) return t.height_m == 0 && n.x == 0 && n.y == 0 && n.z == 0;
    return t.height_m >= j.height_min_m && t.height_m <= j.height_max_m &&
        std::abs(n.x * n.x + n.y * n.y + n.z * n.z - 1) <= .001f;
}
struct Staging {
    std::filesystem::path directory, file;
    ~Staging() {
        std::error_code ec;
        if (!file.empty()) std::filesystem::remove(file, ec);
        if (!directory.empty()) std::filesystem::remove(directory, ec);
    }
    bool create(const std::string &path) {
        static std::atomic<std::uint64_t> serial{0};
        for (int i = 0; i < 16; ++i) {
            auto candidate = path + ".tmp." + std::to_string(Clock::now().time_since_epoch().count()) +
                "." + std::to_string(serial.fetch_add(1));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) {
                directory = candidate; file = directory / "face"; return true;
            }
            if (ec) return false;
        }
        return false;
    }
};
} // namespace

bool validate_projected_face(const FaceJob &j, const FacePatch &p, Error &e,
                              const BuildControl &control) {
    e = {};
    if (!current(j, control, e)) return false;
    FacePatch wanted;
    if (!expected(j, wanted, e)) return false;
    if (header(p) != header(wanted) || p.texels.size() != std::size_t(wanted.layout.width)*wanted.layout.height)
        return fail(e, ErrorCode::ArtifactFailure, "incomplete/mismatched projected face");
    for (std::size_t i=0; i<p.texels.size(); ++i) {
        if (!(i&4095) && !current(j, control, e)) return false;
        if (!valid_texel(p.texels[i], j))
            return fail(e, ErrorCode::ArtifactFailure, "invalid projected face texel");
    }
    return current(j, control, e);
}

bool load_projected_face(const std::string &path, const FaceJob &j, FacePatch &out, Error &e,
                         const BuildControl &control) {
    e = {};
    if (!current(j, control, e)) return false;
    try {
        FacePatch candidate;
        if (!expected(j, candidate, e)) return false;
        const auto h = header(candidate);
        const auto size = kHeaderBytes + std::size_t(candidate.layout.width) * candidate.layout.height * kRecordBytes;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return fail(e, ErrorCode::ArtifactFailure, "finite face cache unavailable");
        if (file.tellg() != std::streamoff(size))
            return fail(e, ErrorCode::ArtifactFailure, "finite face cache size mismatch");
        // Admit size and the complete physical recipe before allocating payload.
        Bytes bytes(kHeaderBytes);
        file.seekg(0); file.read(reinterpret_cast<char *>(bytes.data()), kHeaderBytes);
        if (!file || !std::equal(h.begin(), h.begin() + kChecksumOffset, bytes.begin()))
            return fail(e, ErrorCode::ArtifactFailure, "finite face cache recipe/header mismatch");
        bytes.resize(size);
        file.read(reinterpret_cast<char *>(bytes.data() + kHeaderBytes), std::streamsize(size - kHeaderBytes));
        file.close();
        if (!file || checksum(bytes) != read64(bytes.data() + kChecksumOffset))
            return fail(e, ErrorCode::ArtifactFailure, "finite face cache checksum/read failure");
        candidate.texels.resize(std::size_t(candidate.layout.width) * candidate.layout.height);
        for (std::size_t i = 0; i < candidate.texels.size(); ++i) {
            if (!(i & 4095) && !current(j, control, e)) return false;
            const auto *p = bytes.data() + kHeaderBytes + i * kRecordBytes;
            auto &t = candidate.texels[i];
            t.height_m = read_float(p);
            t.normal_uvn = {read_float(p + 4), read_float(p + 8), read_float(p + 12)};
            t.coverage = read32(p + 16);
            if (!valid_texel(t, j)) return fail(e, ErrorCode::ArtifactFailure, "invalid cached face texel");
        }
        if (!current(j, control, e)) return false;
        out = std::move(candidate);
        return true;
    } catch (const std::bad_alloc &) {
        return fail(e, ErrorCode::LimitExceeded, "face cache read allocation failed");
    }
}
bool save_projected_face(const std::string &path, const FaceJob &j, const FacePatch &p, Error &e,
                         const BuildControl &control) {
    e = {};
    if (!current(j, control, e)) return false;
    try {
        FacePatch wanted;
        if (!expected(j, wanted, e)) return false;
        auto bytes = header(p);
        if (bytes != header(wanted) || p.texels.size() != std::size_t(wanted.layout.width) * wanted.layout.height)
            return fail(e, ErrorCode::ArtifactFailure, "cannot cache incomplete/mismatched face");
        bytes.reserve(kHeaderBytes + p.texels.size() * kRecordBytes);
        for (std::size_t i = 0; i < p.texels.size(); ++i) {
            if (!(i & 4095) && !current(j, control, e)) return false;
            const auto &t = p.texels[i];
            if (!valid_texel(t, j)) return fail(e, ErrorCode::ArtifactFailure, "cannot cache invalid face texel");
            f32(bytes, t.height_m); f32(bytes, t.normal_uvn.x); f32(bytes, t.normal_uvn.y);
            f32(bytes, t.normal_uvn.z); u32(bytes, t.coverage);
        }
        const auto sum = checksum(bytes);
        for (unsigned b = 0; b < 8; ++b) bytes[kChecksumOffset + b] = std::uint8_t(sum >> (8 * b));
        if (!current(j, control, e)) return false;
        Staging staging;
        if (!staging.create(path)) return fail(e, ErrorCode::ArtifactFailure, "face cache staging unavailable");
        std::ofstream file(staging.file, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        file.flush(); file.close();
        if (!file) return fail(e, ErrorCode::ArtifactFailure, "face cache write failed");
        if (!current(j, control, e)) return false;
        if (!part_asset::replace_file_atomic(staging.file.string(), path))
            return fail(e, ErrorCode::ArtifactFailure, "face cache atomic replacement failed");
        return current(j, control, e);
    } catch (const std::bad_alloc &) {
        return fail(e, ErrorCode::LimitExceeded, "face cache write allocation failed");
    }
}
bool load_or_project_face(const std::string &path, const FaceJob &j, const SolidFaceProjector &project,
                          FacePatch &out, FaceCacheStats &stats, Error &e, const BuildControl &control) {
    stats = {};
    FacePatch candidate;
    auto start = Clock::now();
    stats.hit = load_projected_face(path, j, candidate, e, control);
    stats.read_ms = elapsed(start);
    if (!stats.hit && e.code != ErrorCode::ArtifactFailure) return false;
    if (!current(j, control, e)) return false;
    if (!stats.hit) {
        if (!project) return fail(e, ErrorCode::Unavailable, "face cache miss requires projector");
        e = {};
        if (!project(j, candidate, stats.projection, e, control)) return false;
        if (!current(j, control, e)) return false;
        start = Clock::now();
        if (!save_projected_face(path, j, candidate, e, control)) return false;
        stats.write_ms = elapsed(start);
    }
    if (!current(j, control, e)) return false;
    out = std::move(candidate);
    e = {};
    return true;
}
} // namespace gpu_meshing
