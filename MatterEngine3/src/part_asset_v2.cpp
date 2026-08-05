#include "part_asset_v2.h"
#include "matter/lod_contract.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cstddef>   // offsetof
#include <cstdlib>   // strtoull, strtod
#include <fstream>
#include <sstream>   // load_static_lod_plan's per-line tokenizer
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Pin the consumed (read-only) TriEx layout the serializer depends on. The TriEx
// write path below copies only the first kTriExPad (92) named-member bytes into a
// memset-zeroed staging struct so re-bakes stay byte-identical (the alignment
// padding after the trailing ao2 would otherwise carry allocator garbage). That
// trick is only correct if ao2 really is the last named member ending at byte 92
// and sizeof(TriEx) is 96 with 4 trailing padding bytes. If MatterSurfaceLib's
// bvh.h ever reshapes TriEx, these fire at compile time so the determinism fix is
// revisited rather than silently corrupting the content-addressed cache.
static_assert(sizeof(TriEx) == 96, "TriEx layout changed: serializer pad/size assumptions broken");
static_assert(offsetof(TriEx, ao2) == 88, "TriEx trailing member moved: named-member extent (92) is stale");

namespace {
template <class T>
void put(std::vector<uint8_t>& b, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
void put_bytes(std::vector<uint8_t>& b, const void* d, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(d);
    b.insert(b.end(), p, p + n);
}
void ensure_parent_dir(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return;
#ifdef _WIN32
    mkdir(path.substr(0, pos).c_str()); // ignore EEXIST (Windows mkdir takes no mode)
#else
    mkdir(path.substr(0, pos).c_str(), 0755); // ignore EEXIST
#endif
}
bool durable_flush(FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
// POSIX requires an explicit parent-directory fsync after rename.  Windows
// uses MoveFileEx(..., MOVEFILE_WRITE_THROUGH) in replace_file_atomic instead.
#ifndef _WIN32
bool fsync_parent_directory(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    const int fd = open(parent.string().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    const int close_result = close(fd);
    return ok && close_result == 0;
}
#endif
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (p > end || static_cast<size_t>(end - p) < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p, sizeof(T)); p += sizeof(T);
        return v;
    }
    const uint8_t* take(size_t n) {
        if (p > end || static_cast<size_t>(end - p) < n) {
            ok = false;
            return nullptr;
        }
        const uint8_t* r = p; p += n; return r;
    }
};
} // namespace

namespace part_asset {

namespace {
bool g_test_fail_post_rename_once = false;
}

uint64_t compute_resolved_hash(const void* source_bytes, size_t source_len,
                               const void* params_bytes, size_t params_len,
                               const uint64_t* child_hashes, size_t child_count) {
    // Fold source, then params, then sorted child hashes into one rolling FNV-1a.
    // The stream order (source -> params -> sorted children) is fixed.
    uint64_t h = 1469598103934665603ull; // FNV offset basis
    auto fold = [&h](const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    fold(source_bytes, source_len);
    fold(params_bytes, params_len);
    std::vector<uint64_t> sorted(child_hashes, child_hashes + child_count);
    std::sort(sorted.begin(), sorted.end()); // order-independent over children
    for (uint64_t c : sorted) fold(&c, sizeof(c));
    return h;
}

std::string cache_path_resolved(uint64_t resolved_hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(resolved_hash));
    return std::string("parts/") + buf + ".part";
}

std::string cache_path_flat(uint64_t resolved_hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(resolved_hash));
    return std::string("parts/") + buf + ".flat.part";
}

std::string cache_path_lods(uint64_t resolved_hash) {
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)resolved_hash);
    return std::string("parts/") + hex + ".lods";
}

bool load_lod_sidecar(const std::string& path, LodVariants& out) {
    std::ifstream in(path);
    if (!in) return false;
    LodVariants v;
    if (!(in >> v.anchor_size)) return false;
    double budget; std::string hex;
    while (in >> budget >> hex) {
        if (hex.size() != 16) return false;
        v.budgets.push_back(budget);
        v.hashes.push_back((uint64_t)strtoull(hex.c_str(), nullptr, 16));
    }
    if (v.hashes.empty()) return false;
    out = std::move(v);
    return true;
}

std::string cache_path_static_lods(uint64_t resolved_hash) {
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)resolved_hash);
    return std::string("parts/") + hex + ".static_lods";
}

// Line format, one per authored level:
//   <hash16> <mask8> <at> <gen>
// `at` is the authored switch-in distance in metres, or -1 when the level
// declared none. `gen` is "-" for no generator, else the generator name and
// its canonical params joined by '|' (canonical JSON never contains a space,
// so the record stays whitespace-delimited and `>>`-readable).
//
// A two-token line is the pre-M3 form and still loads, with at = -1 and no
// generator -- which is exactly "this plan does not drive the ladder".
bool save_static_lod_plan(const std::string& path, const StaticLodPlan& plan) {
    const size_t n = plan.level_hashes.size();
    if (plan.level_exclude_masks.size() != n) return false;
    if (!plan.level_at.empty()  && plan.level_at.size()  != n) return false;
    if (!plan.level_gen.empty() && plan.level_gen.size() != n) return false;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        for (size_t i = 0; i < n; ++i) {
            char hhex[17], mhex[9];
            snprintf(hhex, sizeof hhex, "%016llx", (unsigned long long)plan.level_hashes[i]);
            snprintf(mhex, sizeof mhex, "%08x", plan.level_exclude_masks[i]);
            const double at = plan.level_at.empty() ? -1.0 : plan.level_at[i];
            std::string gen = plan.level_gen.empty() ? std::string() : plan.level_gen[i];
            for (char& c : gen) if (c == ' ') c = '|';
            if (gen.empty()) gen = "-";
            char atbuf[40];
            snprintf(atbuf, sizeof atbuf, "%.17g", at);
            out << hhex << " " << mhex << " " << atbuf << " " << gen << "\n";
        }
        if (!out.good()) return false;
    }
    return replace_file_atomic(tmp, path);
}

bool load_static_lod_plan(const std::string& path, StaticLodPlan& out) {
    std::ifstream in(path);
    if (!in) return false;
    StaticLodPlan plan;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string hhex, mhex, atstr, gen;
        if (!(ls >> hhex >> mhex)) return false;
        if (hhex.size() != 16 || mhex.size() != 8) return false;
        plan.level_hashes.push_back((uint64_t)strtoull(hhex.c_str(), nullptr, 16));
        plan.level_exclude_masks.push_back((uint32_t)strtoull(mhex.c_str(), nullptr, 16));
        double at = -1.0;
        if (ls >> atstr) at = strtod(atstr.c_str(), nullptr);
        plan.level_at.push_back(at);
        if (ls >> gen && gen != "-") {
            for (char& c : gen) if (c == '|') c = ' ';
            plan.level_gen.push_back(gen);
        } else {
            plan.level_gen.push_back(std::string());
        }
    }
    if (plan.level_hashes.empty()) return false;
    out = std::move(plan);
    return true;
}

std::string cache_path_hints(uint64_t resolved_hash) {
    char buf[64];
    snprintf(buf, sizeof buf, "parts/%016llx.hints",
             (unsigned long long)resolved_hash);
    return buf;
}

bool save_flatten_hints(const std::string& path, const FlattenHints& hints) {
    std::ofstream out(path);
    if (!out) return false;
    for (const auto& [idx, px] : hints.child_px)
        out << idx << " " << px << "\n";
    return (bool)out;
}

bool load_flatten_hints(const std::string& path, FlattenHints& out_hints) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    uint32_t idx; float px;
    while (ifs >> idx >> px) out_hints.child_px[idx] = px;
    if (ifs.fail() && !ifs.eof()) { out_hints.child_px.clear(); return false; }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers: write/read the common body (materials, BLAS, instances,
// children, top-level lods). The v3 writer calls save_common_body and then
// appends the cluster table before finalizing the header.
// ─────────────────────────────────────────────────────────────────────────────

// Appends the common serialization body (materials → BLAS → internal instances
// → children → top-level lods) into `body`. Returns false on a dangling handle.
static bool append_common_body(std::vector<uint8_t>& body,
                               const BLASManager& blas,
                               const TLASManager& tlas,
                               const ChildInstance* children, size_t child_count,
                               const LodLevels& lods,
                               std::unordered_map<BLASHandle, uint32_t>& handle_to_index_out) {
    // --- Materials ---
    // Static count, not MaterialRegistryCount(): per-world dynamic entries
    // (defineMaterial, chart-VT spec Phase 3) append after the 30 builtins and
    // must not change a baked part's bytes — otherwise declaring a material
    // would invalidate every cached artifact. The frozen builtin table is what
    // this block pins; dynamic ids resolve at render time from the live registry.
    put<uint32_t>(body, MaterialRegistrySchemaVersion());
    const uint32_t mcount = static_cast<uint32_t>(MaterialRegistryStaticCount());
    put<uint32_t>(body, mcount);
    for (uint32_t i = 0; i < mcount; ++i)
        put_bytes(body, MaterialRegistryGet(static_cast<int>(i)), sizeof(MaterialDef));

    // --- BLAS table --- (unchanged from v1; index == position in entries_)
    const auto& entries = blas.get_entries();
    put<uint32_t>(body, static_cast<uint32_t>(entries.size()));
    handle_to_index_out.clear();
    for (uint32_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        handle_to_index_out[e->handle] = i;
        const uint32_t tri_count  = static_cast<uint32_t>(e->mesh->triCount);
        const uint32_t nodes_used = e->bvh->nodesUsed;
        const TriEx* triex_src = (!e->tri_extra.empty() && e->tri_extra.size() == tri_count)
                                     ? e->tri_extra.data() : e->mesh->triEx;
        const uint32_t has_triex  = triex_src ? 1u : 0u;
        put<uint32_t>(body, e->hash);
        put<uint32_t>(body, e->ref_count);
        put<uint32_t>(body, tri_count);
        put<uint32_t>(body, nodes_used);
        put<uint32_t>(body, has_triex);
        put_bytes(body, e->triangles.data(), tri_count * sizeof(Tri));
        if (has_triex) {
            // Serialize TriEx through a staging copy whose trailing alignment
            // padding (the 4 bytes after ao2, since sizeof(TriEx)==96 but the named
            // members end at 92) is explicitly zeroed. TriEx is trivially copyable,
            // but the engine fills mesh->triEx via element-wise copy assignment into
            // a MALLOC64 buffer; in practice the compiler may copy only the named
            // members and leave those padding bytes as allocator garbage. Writing
            // them verbatim made otherwise-identical bakes byte-differ and broke the
            // content-addressed cache (the resolved-hash path re-bakes and expects
            // identical bytes). Zeroing the padding here normalizes that without
            // touching the read-only mesher. Tri has no such gap in its serialized
            // form, so it is written directly above.
            constexpr size_t kTriExPad = 92; // bytes occupied by named members
            for (uint32_t t = 0; t < tri_count; ++t) {
                TriEx staged;
                std::memset(&staged, 0, sizeof(TriEx));
                std::memcpy(&staged, &triex_src[t], kTriExPad);
                put_bytes(body, &staged, sizeof(TriEx));
            }
        }
        put_bytes(body, e->bvh->bvhNode, nodes_used * sizeof(BVHNode));
        put_bytes(body, e->bvh->triIdx,  tri_count  * sizeof(uint));
    }

    // --- Internal instances --- (unchanged from v1)
    const auto& recs = tlas.get_draw_records();
    put<uint32_t>(body, static_cast<uint32_t>(recs.size()));
    for (const auto& r : recs) {
        auto it = handle_to_index_out.find(r.blas_handle);
        if (it == handle_to_index_out.end()) return false; // dangling handle
        put<uint32_t>(body, it->second);
        put<uint32_t>(body, r.material_id);
        put_bytes(body, r.transform.m, 16 * sizeof(float));
    }

    // --- Child instances --- (NEW; references to other parts by resolved hash)
    put<uint32_t>(body, static_cast<uint32_t>(child_count));
    for (size_t i = 0; i < child_count; ++i) {
        put<uint64_t>(body, children[i].child_resolved_hash);
        put_bytes(body, children[i].transform, 16 * sizeof(float));
    }

    // --- LOD levels --- (NEW; ordered, may be empty)
    put<uint32_t>(body, static_cast<uint32_t>(lods.size()));
    for (const auto& lvl : lods) {
        put<float>(body, lvl.screen_size_threshold);
        put<uint32_t>(body, static_cast<uint32_t>(lvl.blas_indices.size()));
        for (uint32_t idx : lvl.blas_indices) put<uint32_t>(body, idx);
    }

    return true;
}

// Finalize and atomically write header + body to path.
static bool write_file_atomic(const std::string& path,
                              uint32_t version,
                              uint64_t resolved_hash,
                              const std::vector<uint8_t>& body) {
    const uint64_t content_hash = fnv1a64(body.data(), body.size());
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, version);
    put<uint64_t>(head, resolved_hash ^ static_cast<uint64_t>(version));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(Tri)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(TriEx)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(BVHNode)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(ChildInstance)));
    put<uint64_t>(head, content_hash);

    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "  save_v2: fopen('%s', 'wb') failed: errno=%d (%s); path is absolute from BakeOptions.parts_dir (or cwd-relative if empty)\n",
                     tmp.c_str(), errno, std::strerror(errno));
        return false;
    }
    const bool wrote = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
                       std::fwrite(body.data(), 1, body.size(), f) == body.size();
    const bool flushed = wrote && durable_flush(f);
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !flushed || !closed) {
        std::fprintf(stderr, "  save_v2: fwrite('%s') failed\n", tmp.c_str());
        if (std::remove(tmp.c_str()) != 0 && errno != ENOENT) return false;
        return false;
    }
    if (!replace_file_atomic(tmp, path)) {
        std::fprintf(stderr, "  save_v2: atomic replace('%s' -> '%s') failed: errno=%d (%s)\n",
                     tmp.c_str(), path.c_str(), errno, std::strerror(errno));
        if (std::remove(tmp.c_str()) != 0 && errno != ENOENT) return false;
        return false;
    }
    return true;
}

// Read and validate the common header; on success sets `r` to point at the body
// (i.e., just past the 40-byte header) and returns the format_version.
// Returns 0 on any failure.
static uint32_t read_and_validate_header(Reader& r,
                                         uint64_t expected_resolved_hash,
                                         uint32_t expected_version,
                                         uint64_t& content_hash_out) {
    const uint32_t magic    = r.get<uint32_t>();
    const uint32_t version  = r.get<uint32_t>();
    const uint64_t rhash_x  = r.get<uint64_t>();
    const uint32_t s_tri    = r.get<uint32_t>();
    const uint32_t s_triex  = r.get<uint32_t>();
    const uint32_t s_node   = r.get<uint32_t>();
    const uint32_t s_child  = r.get<uint32_t>();
    const uint64_t content  = r.get<uint64_t>();
    if (!r.ok) return 0;
    if (magic   != kMagic)                  return 0;
    if (version != expected_version)         return 0;
    if (s_tri   != sizeof(Tri))             return 0;
    if (s_triex != sizeof(TriEx))           return 0;
    if (s_node  != sizeof(BVHNode))         return 0;
    if (s_child != sizeof(ChildInstance))   return 0;
    const uint64_t resolved = rhash_x ^ static_cast<uint64_t>(version);
    if (resolved != expected_resolved_hash) return 0;
    content_hash_out = content;
    return version;
}

bool is_cache_artifact_header_compatible(
    const std::string& path, uint64_t expected_resolved_hash,
    uint32_t expected_format_version, CacheArtifactProbeStats* stats) {
    if (stats) *stats = CacheArtifactProbeStats{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t header[40];
    if (std::fread(header, 1, sizeof(header), f) != sizeof(header)) {
        std::fclose(f);
        return false;
    }
    Reader reader{header, header + sizeof(header)};
    uint64_t content_hash_ignored = 0;
    if (!read_and_validate_header(reader, expected_resolved_hash,
                                  expected_format_version,
                                  content_hash_ignored)) {
        std::fclose(f);
        return false;
    }
    const size_t material_prefix_size =
        2u * sizeof(uint32_t) +
        static_cast<size_t>(MaterialRegistryStaticCount()) * sizeof(MaterialDef);
    std::vector<uint8_t> prefix(material_prefix_size);
    const size_t got = std::fread(prefix.data(), 1, prefix.size(), f);
    std::fclose(f);
    if (stats) {
        stats->max_read_chunk = std::max(sizeof(header), got);
        stats->body_bytes = got;
        stats->retained_material_bytes = got;
    }
    if (got != material_prefix_size) return false;
    Reader material_reader{prefix.data(), prefix.data() + prefix.size()};
    const uint32_t material_schema = material_reader.get<uint32_t>();
    const uint32_t material_count = material_reader.get<uint32_t>();
    if (!material_reader.ok ||
        material_schema != MaterialRegistrySchemaVersion() ||
        material_count != static_cast<uint32_t>(MaterialRegistryStaticCount()))
        return false;
    for (uint32_t i = 0; i < material_count; ++i) {
        const uint8_t* serialized = material_reader.take(sizeof(MaterialDef));
        if (!material_reader.ok ||
            std::memcmp(serialized, MaterialRegistryGet(static_cast<int>(i)),
                        sizeof(MaterialDef)) != 0)
            return false;
    }
    return true;
}

// Part v2 must be completely syntactically valid before it acquires any
// caller-owned renderer state.  Keep the parsed common body in ordinary owned
// vectors first; only publish it after the trailer grammar has consumed EOF.
struct ParsedBlasEntry {
    uint32_t hash = 0;
    uint32_t ref_count = 0;
    std::vector<Tri> triangles;
    std::vector<TriEx> tri_extra;
    std::vector<BVHNode> nodes;
    std::vector<uint> tri_indices;
};
struct ParsedDrawInstance {
    uint32_t blas_index = 0;
    uint32_t material_id = 0;
    // Was Matrix4x4 before the Phase 2 MathLib collapse; mm::Mat4 has the same
    // row-major float[16] layout and identity default, so this is a rename.
    mm::Mat4 transform;
};
struct ParsedCommonBody {
    std::vector<ParsedBlasEntry> blas_entries;
    std::vector<ParsedDrawInstance> instances;
    std::vector<ChildInstance> children;
    LodLevels lods;
};

constexpr uint32_t kMaxPartBlasEntries = 65536;
constexpr uint32_t kMaxPartInternalInstances = 1u << 20;
constexpr uint32_t kMaxPartChildren = 1u << 20;
constexpr uint32_t kMaxPartLodLevels = 64;

template <class T>
static bool copy_array(Reader& r, uint32_t count, std::vector<T>& out) {
    if (count > static_cast<uint64_t>(r.end - r.p) / sizeof(T)) {
        r.ok = false;
        return false;
    }
    const uint8_t* data = r.take(static_cast<size_t>(count) * sizeof(T));
    if (!r.ok) return false;
    out.resize(count);
    if (count != 0) std::memcpy(out.data(), data, out.size() * sizeof(T));
    return true;
}

// Syntactic preflight for the common body.  On success r.p is the exact first
// byte after the LOD block, which is the only valid start of an EMIT/ANLK
// suffix.  This function has no BLAS/TLAS side effects.
static bool parse_common_body(Reader& r, ParsedCommonBody& out,
                              PartAssetLoadFailure* failure = nullptr,
                              std::string* reason = nullptr) {
    out = {};

    // --- Materials (validate against the live registry) ---
    const uint32_t material_schema = r.get<uint32_t>();
    if (!r.ok) return false;
    if (material_schema != MaterialRegistrySchemaVersion()) {
        if (failure) *failure = PartAssetLoadFailure::MaterialSchema;
        if (reason) *reason = "material schema mismatch; rebake";
        return false;
    }
    const uint32_t mcount = r.get<uint32_t>();
    if (!r.ok) return false;
    if (static_cast<int>(mcount) != MaterialRegistryStaticCount()) return false;
    for (uint32_t i = 0; i < mcount; ++i) {
        const uint8_t* md = r.take(sizeof(MaterialDef));
        if (!r.ok) return false;
        if (std::memcmp(md, MaterialRegistryGet(static_cast<int>(i)), sizeof(MaterialDef)) != 0)
            return false;
    }

    // --- BLAS table ---
    const uint32_t blas_count = r.get<uint32_t>();
    if (!r.ok || blas_count > kMaxPartBlasEntries ||
        blas_count > static_cast<uint64_t>(r.end - r.p) / (5 * sizeof(uint32_t))) return false;
    out.blas_entries.reserve(blas_count);
    for (uint32_t i = 0; i < blas_count; ++i) {
        ParsedBlasEntry entry;
        entry.hash                = r.get<uint32_t>();
        entry.ref_count           = r.get<uint32_t>();
        const uint32_t tri_count  = r.get<uint32_t>();
        const uint32_t nodes_used = r.get<uint32_t>();
        const uint32_t has_triex  = r.get<uint32_t>();
        if (!r.ok || tri_count == 0 || tri_count > static_cast<uint32_t>(INT32_MAX) ||
            nodes_used == 0 || has_triex > 1 ||
            nodes_used > static_cast<uint64_t>(tri_count) * 2u + 1u ||
            !copy_array(r, tri_count, entry.triangles) ||
            (has_triex && !copy_array(r, tri_count, entry.tri_extra)) ||
            !copy_array(r, nodes_used, entry.nodes) ||
            !copy_array(r, tri_count, entry.tri_indices)) return false;
        for (uint tri_index : entry.tri_indices)
            if (tri_index >= tri_count) return false;
        for (uint32_t node_index = 0; node_index < nodes_used; ++node_index) {
            const BVHNode& node = entry.nodes[node_index];
            if (node.triCount != 0) {
                if (node.leftFirst > tri_count || node.triCount > tri_count - node.leftFirst)
                    return false;
            } else if (nodes_used < 2 || node.leftFirst > nodes_used - 2) {
                return false;
            }
        }
        out.blas_entries.push_back(std::move(entry));
    }

    // --- Internal instances ---
    const uint32_t inst_count = r.get<uint32_t>();
    if (!r.ok || inst_count > kMaxPartInternalInstances ||
        inst_count > static_cast<uint64_t>(r.end - r.p) / (2 * sizeof(uint32_t) + 16 * sizeof(float))) return false;
    out.instances.reserve(inst_count);
    for (uint32_t i = 0; i < inst_count; ++i) {
        ParsedDrawInstance instance;
        instance.blas_index = r.get<uint32_t>();
        instance.material_id = r.get<uint32_t>();
        const uint8_t* tf         = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        if (instance.blas_index >= blas_count) return false;
        std::memcpy(instance.transform.m, tf, 16 * sizeof(float));
        out.instances.push_back(instance);
    }

    // --- Child instances (passive — returned to caller) ---
    const uint32_t child_count = r.get<uint32_t>();
    if (!r.ok || child_count > kMaxPartChildren ||
        child_count > static_cast<uint64_t>(r.end - r.p) / (sizeof(uint64_t) + 16 * sizeof(float))) return false;
    out.children.reserve(child_count);
    for (uint32_t i = 0; i < child_count; ++i) {
        ChildInstance ci{};
        ci.child_resolved_hash = r.get<uint64_t>();
        const uint8_t* tf = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        std::memcpy(ci.transform, tf, 16 * sizeof(float));
        out.children.push_back(ci);
    }

    // --- LOD levels (passive — returned to caller) ---
    const uint32_t level_count = r.get<uint32_t>();
    if (!r.ok || level_count > kMaxPartLodLevels ||
        level_count > static_cast<uint64_t>(r.end - r.p) / (sizeof(float) + sizeof(uint32_t))) return false;
    out.lods.reserve(level_count);
    for (uint32_t i = 0; i < level_count; ++i) {
        LodLevel lvl;
        lvl.screen_size_threshold = r.get<float>();
        const uint32_t idx_count  = r.get<uint32_t>();
        if (!r.ok || idx_count > static_cast<uint64_t>(r.end - r.p) / sizeof(uint32_t)) return false;
        lvl.blas_indices.reserve(idx_count);
        for (uint32_t j = 0; j < idx_count; ++j) {
            const uint32_t idx = r.get<uint32_t>();
            if (!r.ok) return false;
            if (idx >= blas_count) return false; // dangling LOD index: regenerate
            lvl.blas_indices.push_back(idx);
        }
        out.lods.push_back(std::move(lvl));
    }
    return r.ok;
}

static bool publish_common_body(const ParsedCommonBody& parsed,
                                BLASManager& blas, TLASManager& tlas,
                                std::vector<ChildInstance>& children_out,
                                LodLevels& lods_out) {
    std::vector<BLASHandle> handles;
    handles.reserve(parsed.blas_entries.size());
    for (const ParsedBlasEntry& entry : parsed.blas_entries) {
        const BLASHandle handle = blas.register_prebuilt(
            entry.triangles.data(),
            entry.tri_extra.empty() ? nullptr : entry.tri_extra.data(),
            static_cast<int>(entry.triangles.size()), entry.nodes.data(),
            static_cast<uint>(entry.nodes.size()), entry.tri_indices.data(),
            entry.hash, entry.ref_count);
        if (handle == INVALID_BLAS_HANDLE) return false;
        handles.push_back(handle);
    }
    if (!parsed.instances.empty()) {
        std::vector<TLASManager::DrawInstance> instances;
        instances.reserve(parsed.instances.size());
        for (const ParsedDrawInstance& input : parsed.instances) {
            TLASManager::DrawInstance output{};
            output.blas_handle = handles[input.blas_index];
            output.material_id = input.material_id;
            output.transform = input.transform;
            instances.push_back(output);
        }
        tlas.draw_batch(instances);
        tlas.build(blas);
    }
    children_out = parsed.children;
    lods_out = parsed.lods;
    return true;
}

// Flat artifacts have their own post-common-body grammar.  Keep their existing
// loader contract while v2 uses the stricter preflight below.
static bool read_common_body(Reader& r, BLASManager& blas, TLASManager& tlas,
                             std::vector<ChildInstance>& children_out,
                             LodLevels& lods_out,
                             std::vector<BLASHandle>& handles_out,
                             PartAssetLoadFailure* failure = nullptr,
                             std::string* reason = nullptr) {
    ParsedCommonBody parsed;
    if (!parse_common_body(r, parsed, failure, reason)) return false;
    handles_out.clear();
    return publish_common_body(parsed, blas, tlas, children_out, lods_out);
}

struct PartV2Preflight {
    std::vector<uint8_t> bytes;
    ParsedCommonBody common;
    size_t suffix_offset = 0;
};

static bool preflight_v2_file(const std::string& path, uint64_t expected_resolved_hash,
                              PartV2Preflight& out, PartAssetLoadFailure* failure,
                              std::string* reason) {
    out = {};
    const auto fail = [failure, reason](PartAssetLoadFailure value, const char* message) {
        if (failure) *failure = value;
        if (reason) *reason = message;
        return false;
    };
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail(PartAssetLoadFailure::Header, "invalid part header");
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 40) { std::fclose(f); return fail(PartAssetLoadFailure::Header, "invalid part header"); }
    out.bytes.resize(static_cast<size_t>(size));
    const bool read_ok = std::fread(out.bytes.data(), 1, out.bytes.size(), f) == out.bytes.size();
    std::fclose(f);
    if (!read_ok) return fail(PartAssetLoadFailure::Header, "invalid part header");
    Reader reader{out.bytes.data(), out.bytes.data() + out.bytes.size()};
    uint64_t content_hash = 0;
    if (!read_and_validate_header(reader, expected_resolved_hash, kFormatVersionV2, content_hash))
        return fail(PartAssetLoadFailure::Header, "invalid part header");
    if (fnv1a64(reader.p, static_cast<size_t>(reader.end - reader.p)) != content_hash)
        return fail(PartAssetLoadFailure::CorruptBody, "corrupt part body");
    if (!parse_common_body(reader, out.common, failure, reason)) {
        if (failure && *failure == PartAssetLoadFailure::None) *failure = PartAssetLoadFailure::CorruptBody;
        if (reason && reason->empty()) *reason = "corrupt part body";
        return false;
    }
    out.suffix_offset = static_cast<size_t>(reader.p - out.bytes.data());
    return true;
}

static bool parse_v2_suffix(const PartV2Preflight& input, uint64_t expected_resolved_hash,
                            bool accept_animation_link,
                            std::vector<VolumeEmitter>* emitters_out,
                            std::optional<PartAnimationLink>* animation_link_out,
                            PartAssetLoadFailure* failure, std::string* reason) {
    if (emitters_out) emitters_out->clear();
    if (animation_link_out) animation_link_out->reset();
    const auto fail = [failure, reason](const char* message) {
        if (failure) *failure = PartAssetLoadFailure::CorruptBody;
        if (reason) *reason = message;
        return false;
    };
    Reader r{input.bytes.data() + input.suffix_offset,
             input.bytes.data() + input.bytes.size()};
    if (r.p != r.end) {
        if (static_cast<size_t>(r.end - r.p) < sizeof(uint32_t)) return fail("unknown part trailer");
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(tag));
        if (tag == 0x454D4954u) { // EMIT
            r.p += sizeof(tag);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok || count > static_cast<uint64_t>(r.end - r.p) / sizeof(VolumeEmitter))
                return fail("corrupt EMIT trailer");
            const uint8_t* bytes = r.take(static_cast<size_t>(count) * sizeof(VolumeEmitter));
            if (!r.ok) return fail("corrupt EMIT trailer");
            if (emitters_out) {
                emitters_out->resize(count);
                if (count != 0) std::memcpy(emitters_out->data(), bytes,
                                            static_cast<size_t>(count) * sizeof(VolumeEmitter));
            }
        }
    }
    // W5's LMSK trailer (per-level child presence) is written after EMIT by the
    // static-lods save path. It is inert here -- the mask is read by the load_v2
    // overload that asks for it -- but it MUST be skipped rather than treated as
    // the start of an ANLK block, or every LMSK part fails to load through this
    // parser as an "unknown part trailer".
    if (r.p != r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(tag));
        if (tag == 0x4C4D534Bu) { // LMSK
            r.p += sizeof(tag);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok || count > static_cast<uint64_t>(r.end - r.p) / sizeof(uint32_t))
                return fail("corrupt LMSK trailer");
            r.take(static_cast<size_t>(count) * sizeof(uint32_t));
            if (!r.ok) return fail("corrupt LMSK trailer");
        }
    }
    // WP-A's CHRT trailer (chart-space VT sidecar) is byte-size framed; inert
    // here -- the chart table is read by the load_v2 overload that asks for it
    // -- but it must be skipped so chart-bearing parts still load through the
    // strict suffix grammar (same rationale as LMSK above).
    if (r.p != r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(tag));
        if (tag == 0x43485254u) { // CHRT
            r.p += sizeof(tag);
            const uint32_t bytes = r.get<uint32_t>();
            if (!r.ok || bytes > static_cast<uint64_t>(r.end - r.p))
                return fail("corrupt CHRT trailer");
            r.take(static_cast<size_t>(bytes));
            if (!r.ok) return fail("corrupt CHRT trailer");
        }
    }
    if (r.p == r.end) return true;
    if (!accept_animation_link) return fail("unknown part trailer");
    if (static_cast<size_t>(r.end - r.p) != 36) return fail("corrupt ANLK trailer");
    if (r.get<uint32_t>() != 0x414E4C4Bu) return fail("unknown part trailer");
    PartAnimationLink link;
    link.version = r.get<uint32_t>();
    link.bundle_required = r.get<uint32_t>();
    link.resolved_hash = r.get<uint64_t>();
    link.nonce_high = r.get<uint64_t>();
    link.nonce_low = r.get<uint64_t>();
    if (!r.ok || r.p != r.end || link.version != 1 || link.bundle_required != 1 ||
        link.resolved_hash != expected_resolved_hash) return fail("corrupt ANLK trailer");
    if (animation_link_out) *animation_link_out = link;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void set_replace_file_atomic_test_post_rename_failure_once() {
    g_test_fail_post_rename_once = true;
}

FileReplaceOutcome replace_file_atomic_detailed(const std::string& source_path,
                                                const std::string& target_path) {
#ifdef _WIN32
    if (MoveFileExA(source_path.c_str(), target_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
        return FileReplaceOutcome::NotReplaced;
#else
    if (std::rename(source_path.c_str(), target_path.c_str()) != 0)
        return FileReplaceOutcome::NotReplaced;
    if (!fsync_parent_directory(target_path))
        return FileReplaceOutcome::ReplacedNotDurabilityConfirmed;
#endif
    if (g_test_fail_post_rename_once) {
        g_test_fail_post_rename_once = false;
        return FileReplaceOutcome::ReplacedNotDurabilityConfirmed;
    }
    return FileReplaceOutcome::ReplacedDurable;
}

bool replace_file_atomic(const std::string& source_path,
                         const std::string& target_path) {
    return replace_file_atomic_detailed(source_path, target_path) ==
           FileReplaceOutcome::ReplacedDurable;
}

bool save_v2(const std::string& path, const BLASManager& blas,
             const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,
             uint64_t resolved_hash) {
    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, children, child_count, lods, h2i))
        return false;
    return write_file_atomic(path, kFormatVersionV2, resolved_hash, body);
}

bool save_v2(const std::string& path, const BLASManager& blas,
             const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,
             const std::vector<VolumeEmitter>& emitters,
             const PartAnimationLink& link,
             uint64_t resolved_hash) {
    if (link.version != 1 || link.bundle_required != 1 ||
        link.resolved_hash != resolved_hash) return false;
    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, children, child_count, lods, h2i))
        return false;
    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u); // EMIT
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
    }
    put<uint32_t>(body, 0x414E4C4Bu); // ANLK
    put<uint32_t>(body, link.version);
    put<uint32_t>(body, link.bundle_required);
    put<uint64_t>(body, link.resolved_hash);
    put<uint64_t>(body, link.nonce_high);
    put<uint64_t>(body, link.nonce_low);
    return write_file_atomic(path, kFormatVersionV2, resolved_hash, body);
}

bool save_v2(const std::string& path, const BLASManager& blas,
             const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,
             const std::vector<VolumeEmitter>& emitters,
             uint64_t resolved_hash) {
    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, children, child_count, lods, h2i))
        return false;
    // Append the EMIT trailer only when there are emitters (backward compat).
    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u);  // "EMIT" tag
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
    }
    return write_file_atomic(path, kFormatVersionV2, resolved_hash, body);
}

bool save_v2(const std::string& path, const BLASManager& blas,
             const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,
             const std::vector<VolumeEmitter>& emitters,
             const std::vector<uint32_t>& child_level_mask,
             uint64_t resolved_hash) {
    // Compat guard: a non-empty mask must be parallel to the children table.
    if (!child_level_mask.empty() && child_level_mask.size() != child_count)
        return false;
    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, children, child_count, lods, h2i))
        return false;
    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u);  // "EMIT" tag
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
    }
    // W5: LMSK trailer — written ONLY when the caller supplies a mask, so a
    // part without `static lods` (child_level_mask always empty from every
    // pre-W5 call site) appends nothing here, same as the EMIT gate above.
    if (!child_level_mask.empty()) {
        put<uint32_t>(body, 0x4C4D534Bu);  // "LMSK" tag
        put<uint32_t>(body, static_cast<uint32_t>(child_level_mask.size()));
        put_bytes(body, child_level_mask.data(),
                  child_level_mask.size() * sizeof(uint32_t));
    }
    return write_file_atomic(path, kFormatVersionV2, resolved_hash, body);
}

bool save_v2(const std::string& path, const BLASManager& blas,
             const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,
             const std::vector<VolumeEmitter>& emitters,
             const std::vector<chart_atlas::ChartAtlasRung>& rung_charts,
             uint64_t resolved_hash) {
    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, children, child_count, lods, h2i))
        return false;
    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u);  // "EMIT" tag
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
    }
    // WP-A: CHRT trailer — written ONLY when charts exist, so chartless parts
    // stay byte-identical (same gate discipline as EMIT/LMSK above). Payload is
    // byte-size framed so chart-unaware readers can skip it wholesale.
    if (!rung_charts.empty()) {
        std::vector<uint8_t> payload;
        chart_atlas::append_chart_rungs(payload, rung_charts);
        put<uint32_t>(body, 0x43485254u);  // "CHRT" tag
        put<uint32_t>(body, static_cast<uint32_t>(payload.size()));
        put_bytes(body, payload.data(), payload.size());
    }
    return write_file_atomic(path, kFormatVersionV2, resolved_hash, body);
}

bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out,
             PartAssetLoadFailure* failure,
             std::string* reason) {
    children_out.clear();
    lods_out.clear();
    if (failure) *failure = PartAssetLoadFailure::None;
    if (reason) reason->clear();
    PartV2Preflight preflight;
    if (!preflight_v2_file(path, expected_resolved_hash, preflight, failure, reason) ||
        !parse_v2_suffix(preflight, expected_resolved_hash, false, nullptr, nullptr,
                         failure, reason)) return false;
    if (!publish_common_body(preflight.common, blas, tlas, children_out, lods_out)) {
        if (failure) *failure = PartAssetLoadFailure::CorruptBody;
        if (reason) *reason = "failed to publish validated part body";
        return false;
    }
    return true;
}

bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out,
             std::vector<VolumeEmitter>& emitters_out,
             std::optional<PartAnimationLink>& animation_link_out,
             PartAssetLoadFailure* failure, std::string* reason) {
    children_out.clear(); lods_out.clear(); emitters_out.clear(); animation_link_out.reset();
    if (failure) *failure = PartAssetLoadFailure::None;
    if (reason) reason->clear();
    PartV2Preflight preflight;
    if (!preflight_v2_file(path, expected_resolved_hash, preflight, failure, reason) ||
        !parse_v2_suffix(preflight, expected_resolved_hash, true, &emitters_out,
                         &animation_link_out, failure, reason)) return false;
    if (!publish_common_body(preflight.common, blas, tlas, children_out, lods_out)) {
        if (failure) *failure = PartAssetLoadFailure::CorruptBody;
        if (reason) *reason = "failed to publish validated part body";
        return false;
    }
    return true;
}

bool load_animation_link(const std::string& path, uint64_t expected_resolved_hash,
                         std::optional<PartAnimationLink>& out) {
    out.reset();
    PartV2Preflight preflight;
    return preflight_v2_file(path, expected_resolved_hash, preflight, nullptr, nullptr) &&
           parse_v2_suffix(preflight, expected_resolved_hash, true, nullptr, &out,
                           nullptr, nullptr);
}

bool load_static_part_snapshot(const std::string& path, uint64_t expected_resolved_hash,
                               uint64_t& fingerprint_out) {
    fingerprint_out = 0;
    PartV2Preflight preflight;
    std::optional<PartAnimationLink> link;
    if (!preflight_v2_file(path, expected_resolved_hash, preflight, nullptr, nullptr) ||
        !parse_v2_suffix(preflight, expected_resolved_hash, true, nullptr, &link,
                         nullptr, nullptr) ||
        link) return false;
    fingerprint_out = fnv1a64(preflight.bytes.data(), preflight.bytes.size());
    return true;
}

bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out,
             std::vector<VolumeEmitter>& emitters_out,
             PartAssetLoadFailure* failure,
             std::string* reason) {
    emitters_out.clear();
    children_out.clear();
    lods_out.clear();
    if (failure) *failure = PartAssetLoadFailure::None;
    if (reason) reason->clear();
    PartV2Preflight preflight;
    if (!preflight_v2_file(path, expected_resolved_hash, preflight, failure, reason) ||
        !parse_v2_suffix(preflight, expected_resolved_hash, false, &emitters_out,
                         nullptr, failure, reason)) return false;
    if (!publish_common_body(preflight.common, blas, tlas, children_out, lods_out)) {
        if (failure) *failure = PartAssetLoadFailure::CorruptBody;
        if (reason) *reason = "failed to publish validated part body";
        return false;
    }
    return true;
}

bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out,
             std::vector<VolumeEmitter>& emitters_out,
             std::vector<uint32_t>& child_level_mask_out,
             PartAssetLoadFailure* failure,
             std::string* reason) {
    emitters_out.clear();
    children_out.clear();
    lods_out.clear();
    child_level_mask_out.clear();
    if (failure) *failure = PartAssetLoadFailure::None;
    if (reason) reason->clear();

    const auto fail = [failure, reason](PartAssetLoadFailure value, const char* message) {
        if (failure) *failure = value;
        if (reason) *reason = message;
        return false;
    };

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail(PartAssetLoadFailure::Header, "invalid part header");
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 40) { std::fclose(f); return fail(PartAssetLoadFailure::Header, "invalid part header"); }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!read_ok) return fail(PartAssetLoadFailure::Header, "invalid part header");

    Reader r{ buf.data(), buf.data() + buf.size() };
    uint64_t content_hash = 0;
    if (!read_and_validate_header(r, expected_resolved_hash, kFormatVersionV2, content_hash))
        return fail(PartAssetLoadFailure::Header, "invalid part header");
    if (fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content_hash)
        return fail(PartAssetLoadFailure::CorruptBody, "corrupt part body");

    std::vector<BLASHandle> handles;
    if (!read_common_body(r, blas, tlas, children_out, lods_out, handles, failure, reason)) {
        if (failure && *failure == PartAssetLoadFailure::None)
            *failure = PartAssetLoadFailure::CorruptBody;
        if (reason && reason->empty()) *reason = "corrupt part body";
        return false;
    }

    // Probe for the optional EMIT trailer (EOF-tolerant; same as the other
    // emitters-reading overload above).
    if (r.p < r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(uint32_t));
        if (tag == 0x454D4954u) {
            r.p += sizeof(uint32_t);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt EMIT trailer");
            const size_t bytes = static_cast<size_t>(count) * sizeof(VolumeEmitter);
            const uint8_t* data = r.take(bytes);
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt EMIT trailer");
            emitters_out.resize(count);
            std::memcpy(emitters_out.data(), data, bytes);
        }
    }

    // W5: probe for the optional LMSK trailer (EOF-tolerant, same pattern).
    // Absent => child_level_mask_out stays empty (compat: all levels).
    if (r.p < r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(uint32_t));
        if (tag == 0x4C4D534Bu) {
            r.p += sizeof(uint32_t);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt LMSK trailer");
            const size_t bytes = static_cast<size_t>(count) * sizeof(uint32_t);
            const uint8_t* data = r.take(bytes);
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt LMSK trailer");
            if (count != children_out.size())
                return fail(PartAssetLoadFailure::CorruptBody,
                            "LMSK trailer size does not match child table");
            child_level_mask_out.resize(count);
            std::memcpy(child_level_mask_out.data(), data, bytes);
        }
    }
    return true;
}

// WP-A: emitters + chart-sidecar overload. Absent CHRT (older or chartless
// parts) => rung_charts_out stays empty (charts = 0, legacy path).
bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out,
             std::vector<VolumeEmitter>& emitters_out,
             std::vector<chart_atlas::ChartAtlasRung>& rung_charts_out,
             PartAssetLoadFailure* failure,
             std::string* reason) {
    emitters_out.clear();
    children_out.clear();
    lods_out.clear();
    rung_charts_out.clear();
    if (failure) *failure = PartAssetLoadFailure::None;
    if (reason) reason->clear();

    const auto fail = [failure, reason](PartAssetLoadFailure value, const char* message) {
        if (failure) *failure = value;
        if (reason) *reason = message;
        return false;
    };

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail(PartAssetLoadFailure::Header, "invalid part header");
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 40) { std::fclose(f); return fail(PartAssetLoadFailure::Header, "invalid part header"); }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!read_ok) return fail(PartAssetLoadFailure::Header, "invalid part header");

    Reader r{ buf.data(), buf.data() + buf.size() };
    uint64_t content_hash = 0;
    if (!read_and_validate_header(r, expected_resolved_hash, kFormatVersionV2, content_hash))
        return fail(PartAssetLoadFailure::Header, "invalid part header");
    if (fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content_hash)
        return fail(PartAssetLoadFailure::CorruptBody, "corrupt part body");

    std::vector<BLASHandle> handles;
    if (!read_common_body(r, blas, tlas, children_out, lods_out, handles, failure, reason)) {
        if (failure && *failure == PartAssetLoadFailure::None)
            *failure = PartAssetLoadFailure::CorruptBody;
        if (reason && reason->empty()) *reason = "corrupt part body";
        return false;
    }

    // Probe for the optional EMIT trailer (EOF-tolerant, same as elsewhere).
    if (r.p < r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(uint32_t));
        if (tag == 0x454D4954u) {
            r.p += sizeof(uint32_t);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt EMIT trailer");
            const size_t bytes = static_cast<size_t>(count) * sizeof(VolumeEmitter);
            const uint8_t* data = r.take(bytes);
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt EMIT trailer");
            emitters_out.resize(count);
            std::memcpy(emitters_out.data(), data, bytes);
        }
    }

    // Skip an optional LMSK trailer (this overload does not surface it).
    if (r.p < r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(uint32_t));
        if (tag == 0x4C4D534Bu) {
            r.p += sizeof(uint32_t);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt LMSK trailer");
            r.take(static_cast<size_t>(count) * sizeof(uint32_t));
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt LMSK trailer");
        }
    }

    // WP-A: probe for the optional CHRT trailer. Absent => charts = 0.
    if (r.p < r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(uint32_t));
        if (tag == 0x43485254u) {
            r.p += sizeof(uint32_t);
            const uint32_t bytes = r.get<uint32_t>();
            if (!r.ok || bytes > static_cast<uint64_t>(r.end - r.p))
                return fail(PartAssetLoadFailure::CorruptBody, "corrupt CHRT trailer");
            const uint8_t* data = r.take(static_cast<size_t>(bytes));
            if (!r.ok) return fail(PartAssetLoadFailure::CorruptBody, "corrupt CHRT trailer");
            if (!chart_atlas::parse_chart_rungs(data, data + bytes, rung_charts_out))
                return fail(PartAssetLoadFailure::CorruptBody, "corrupt CHRT trailer");
        }
    }
    return true;
}

bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  const std::vector<FlatInstanceRef>& instance_refs,
                  uint64_t resolved_hash) {
    for (const auto& cluster : clusters)
        if (cluster.lods.size() > matter::kMaxSerializedLodLevels) return false;

    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    // v7 body = v6 layout plus the enforced shared LOD capacity.
    if (!append_common_body(body, blas, tlas, nullptr, 0, LodLevels{}, h2i))
        return false;

    // --- Cluster table ---
    put<uint32_t>(body, static_cast<uint32_t>(clusters.size()));
    for (const auto& c : clusters) {
        put_bytes(body, c.aabb_min, 3 * sizeof(float));
        put_bytes(body, c.aabb_max, 3 * sizeof(float));
        put<uint32_t>(body, c.segment);                         // v6: segment tag
        put<uint32_t>(body, static_cast<uint32_t>(c.lods.size()));
        for (const auto& lvl : c.lods) {
            put<float>(body, lvl.screen_size_threshold);
            put<uint32_t>(body, static_cast<uint32_t>(lvl.blas_indices.size()));
            for (uint32_t idx : lvl.blas_indices) put<uint32_t>(body, idx);
        }
    }

    // --- Instance refs (v6 trailer) ---
    // Each entry is a placement of a child part that the flatten decision left
    // as an instance boundary; runtime consumer expands to a world instance.
    // _pad is NOT serialized.
    put<uint32_t>(body, static_cast<uint32_t>(instance_refs.size()));
    for (const auto& r : instance_refs) {
        put<uint64_t>(body, r.child_resolved_hash);
        put_bytes(body, r.transform, 16 * sizeof(float));
        put<float>(body, r.inline_cutover);                     // v6: inline cutover
    }

    return write_file_atomic(path, kFormatVersionFlat, resolved_hash, body);
}

bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  uint64_t resolved_hash) {
    return save_flat_v3(path, blas, tlas, clusters,
                        std::vector<FlatInstanceRef>{}, resolved_hash);
}

bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  const std::vector<FlatInstanceRef>& instance_refs,
                  uint64_t resolved_hash,
                  const std::vector<VolumeEmitter>& emitters) {
    for (const auto& cluster : clusters)
        if (cluster.lods.size() > matter::kMaxSerializedLodLevels) return false;

    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, nullptr, 0, LodLevels{}, h2i))
        return false;

    put<uint32_t>(body, static_cast<uint32_t>(clusters.size()));
    for (const auto& c : clusters) {
        put_bytes(body, c.aabb_min, 3 * sizeof(float));
        put_bytes(body, c.aabb_max, 3 * sizeof(float));
        put<uint32_t>(body, c.segment);
        put<uint32_t>(body, static_cast<uint32_t>(c.lods.size()));
        for (const auto& lvl : c.lods) {
            put<float>(body, lvl.screen_size_threshold);
            put<uint32_t>(body, static_cast<uint32_t>(lvl.blas_indices.size()));
            for (uint32_t idx : lvl.blas_indices) put<uint32_t>(body, idx);
        }
    }

    put<uint32_t>(body, static_cast<uint32_t>(instance_refs.size()));
    for (const auto& r : instance_refs) {
        put<uint64_t>(body, r.child_resolved_hash);
        put_bytes(body, r.transform, 16 * sizeof(float));
        put<float>(body, r.inline_cutover);
    }

    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u);
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
    }

    return write_file_atomic(path, kFormatVersionFlat, resolved_hash, body);
}

bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out,
                  std::vector<FlatInstanceRef>& instance_refs_out) {
    clusters_out.clear();
    instance_refs_out.clear();

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 40) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data() + buf.size() };
    uint64_t content_hash = 0;
    if (!read_and_validate_header(r, expected_resolved_hash, kFormatVersionFlat, content_hash))
        return false;
    if (fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content_hash) return false;

    std::vector<ChildInstance> children_ignored;
    LodLevels lods_ignored;
    std::vector<BLASHandle> handles;
    if (!read_common_body(r, blas, tlas, children_ignored, lods_ignored, handles))
        return false;

    const uint32_t blas_count = static_cast<uint32_t>(blas.get_entries().size());

    // --- Cluster table ---
    const uint32_t cluster_count = r.get<uint32_t>();
    if (!r.ok) return false;
    clusters_out.reserve(cluster_count);
    for (uint32_t ci = 0; ci < cluster_count; ++ci) {
        FlatCluster fc;
        const uint8_t* amin = r.take(3 * sizeof(float));
        const uint8_t* amax = r.take(3 * sizeof(float));
        if (!r.ok) return false;
        std::memcpy(fc.aabb_min, amin, 3 * sizeof(float));
        std::memcpy(fc.aabb_max, amax, 3 * sizeof(float));
        fc.segment = r.get<uint32_t>();                        // v6: segment tag
        const uint32_t level_count = r.get<uint32_t>();
        if (!r.ok) return false;
        if (level_count > matter::kMaxSerializedLodLevels) return false;
        fc.lods.reserve(level_count);
        for (uint32_t li = 0; li < level_count; ++li) {
            LodLevel lvl;
            lvl.screen_size_threshold = r.get<float>();
            const uint32_t idx_count = r.get<uint32_t>();
            if (!r.ok) return false;
            lvl.blas_indices.reserve(idx_count);
            for (uint32_t j = 0; j < idx_count; ++j) {
                const uint32_t idx = r.get<uint32_t>();
                if (!r.ok) return false;
                if (idx >= blas_count) return false; // dangling cluster LOD index
                lvl.blas_indices.push_back(idx);
            }
            fc.lods.push_back(std::move(lvl));
        }
        clusters_out.push_back(std::move(fc));
    }
    if (!r.ok) return false;

    // --- Instance refs trailer (v6) ---
    // Every valid v6 flat has this trailer, even if empty. If the reader hits
    // EOF before we can read the ref_count, the artifact is malformed.
    const uint32_t ref_count = r.get<uint32_t>();
    if (!r.ok) return false;
    instance_refs_out.reserve(ref_count);
    for (uint32_t i = 0; i < ref_count; ++i) {
        FlatInstanceRef ref{};
        ref.child_resolved_hash = r.get<uint64_t>();
        const uint8_t* tf = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        std::memcpy(ref.transform, tf, 16 * sizeof(float));
        ref.inline_cutover = r.get<float>();                   // v6: inline cutover (_pad not serialized)
        instance_refs_out.push_back(ref);
    }
    return r.ok;
}

bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out) {
    std::vector<FlatInstanceRef> refs_ignored;
    return load_flat_v3(path, expected_resolved_hash, blas, tlas, clusters_out,
                        refs_ignored);
}

bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out,
                  std::vector<FlatInstanceRef>& instance_refs_out,
                  std::vector<VolumeEmitter>& emitters_out) {
    clusters_out.clear();
    instance_refs_out.clear();
    emitters_out.clear();

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 40) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data() + buf.size() };
    uint64_t content_hash = 0;
    if (!read_and_validate_header(r, expected_resolved_hash, kFormatVersionFlat, content_hash))
        return false;
    if (fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content_hash) return false;

    std::vector<ChildInstance> children_ignored;
    LodLevels lods_ignored;
    std::vector<BLASHandle> handles;
    if (!read_common_body(r, blas, tlas, children_ignored, lods_ignored, handles))
        return false;

    const uint32_t blas_count = static_cast<uint32_t>(blas.get_entries().size());

    const uint32_t cluster_count = r.get<uint32_t>();
    if (!r.ok) return false;
    clusters_out.reserve(cluster_count);
    for (uint32_t ci = 0; ci < cluster_count; ++ci) {
        FlatCluster fc;
        const uint8_t* amin = r.take(3 * sizeof(float));
        const uint8_t* amax = r.take(3 * sizeof(float));
        if (!r.ok) return false;
        std::memcpy(fc.aabb_min, amin, 3 * sizeof(float));
        std::memcpy(fc.aabb_max, amax, 3 * sizeof(float));
        fc.segment = r.get<uint32_t>();
        const uint32_t level_count = r.get<uint32_t>();
        if (!r.ok) return false;
        if (level_count > matter::kMaxSerializedLodLevels) return false;
        fc.lods.reserve(level_count);
        for (uint32_t li = 0; li < level_count; ++li) {
            LodLevel lvl;
            lvl.screen_size_threshold = r.get<float>();
            const uint32_t idx_count = r.get<uint32_t>();
            if (!r.ok) return false;
            lvl.blas_indices.reserve(idx_count);
            for (uint32_t j = 0; j < idx_count; ++j) {
                const uint32_t idx = r.get<uint32_t>();
                if (!r.ok) return false;
                if (idx >= blas_count) return false;
                lvl.blas_indices.push_back(idx);
            }
            fc.lods.push_back(std::move(lvl));
        }
        clusters_out.push_back(std::move(fc));
    }
    if (!r.ok) return false;

    const uint32_t ref_count = r.get<uint32_t>();
    if (!r.ok) return false;
    instance_refs_out.reserve(ref_count);
    for (uint32_t i = 0; i < ref_count; ++i) {
        FlatInstanceRef ref{};
        ref.child_resolved_hash = r.get<uint64_t>();
        const uint8_t* tf = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        std::memcpy(ref.transform, tf, 16 * sizeof(float));
        ref.inline_cutover = r.get<float>();
        instance_refs_out.push_back(ref);
    }
    if (!r.ok) return false;

    // Probe for the optional EMIT trailer (EOF-tolerant).
    if (r.p < r.end && static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        uint32_t tag = 0;
        std::memcpy(&tag, r.p, sizeof(uint32_t));
        if (tag == 0x454D4954u) {
            r.p += sizeof(uint32_t);
            const uint32_t count = r.get<uint32_t>();
            if (!r.ok) return false;
            const size_t bytes = static_cast<size_t>(count) * sizeof(VolumeEmitter);
            const uint8_t* data = r.take(bytes);
            if (!r.ok) return false;
            emitters_out.resize(count);
            std::memcpy(emitters_out.data(), data, bytes);
        }
    }
    return true;
}

uint32_t peek_format_version(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint32_t magic = 0, version = 0;
    bool ok = std::fread(&magic,   sizeof(uint32_t), 1, f) == 1 &&
              std::fread(&version, sizeof(uint32_t), 1, f) == 1;
    std::fclose(f);
    if (!ok || magic != kMagic) return 0;
    return version;
}

} // namespace part_asset
