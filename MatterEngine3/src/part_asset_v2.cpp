#include "part_asset_v2.h"
#include "matter/lod_contract.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cstddef>   // offsetof
#include <cstdlib>   // strtoull
#include <fstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
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
    put<uint32_t>(body, MaterialRegistrySchemaVersion());
    const uint32_t mcount = static_cast<uint32_t>(MaterialRegistryCount());
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
    bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
              std::fwrite(body.data(), 1, body.size(), f) == body.size();
    std::fclose(f);
    if (!ok) {
        std::fprintf(stderr, "  save_v2: fwrite('%s') failed\n", tmp.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    if (!replace_file_atomic(tmp, path)) {
        std::fprintf(stderr, "  save_v2: atomic replace('%s' -> '%s') failed: errno=%d (%s)\n",
                     tmp.c_str(), path.c_str(), errno, std::strerror(errno));
        std::remove(tmp.c_str());
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
        static_cast<size_t>(MaterialRegistryCount()) * sizeof(MaterialDef);
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
        material_count != static_cast<uint32_t>(MaterialRegistryCount()))
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

// Read the common body sections (materials, BLAS, instances, children, lods)
// from a Reader that is positioned just past the header. Populates the managers
// and out-params; returns false on any corruption.
static bool read_common_body(Reader& r,
                             BLASManager& blas, TLASManager& tlas,
                             std::vector<ChildInstance>& children_out,
                             LodLevels& lods_out,
                             std::vector<BLASHandle>& handles_out,
                             PartAssetLoadFailure* failure = nullptr,
                             std::string* reason = nullptr) {
    children_out.clear();
    lods_out.clear();
    handles_out.clear();

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
    if (static_cast<int>(mcount) != MaterialRegistryCount()) return false;
    for (uint32_t i = 0; i < mcount; ++i) {
        const uint8_t* md = r.take(sizeof(MaterialDef));
        if (!r.ok) return false;
        if (std::memcmp(md, MaterialRegistryGet(static_cast<int>(i)), sizeof(MaterialDef)) != 0)
            return false;
    }

    // --- BLAS table ---
    const uint32_t blas_count = r.get<uint32_t>();
    if (!r.ok) return false;
    handles_out.resize(blas_count, INVALID_BLAS_HANDLE);
    for (uint32_t i = 0; i < blas_count; ++i) {
        const uint32_t hash       = r.get<uint32_t>();
        const uint32_t ref_count  = r.get<uint32_t>();
        const uint32_t tri_count  = r.get<uint32_t>();
        const uint32_t nodes_used = r.get<uint32_t>();
        const uint32_t has_triex  = r.get<uint32_t>();
        if (!r.ok) return false;
        const Tri*     tris  = reinterpret_cast<const Tri*>(r.take(tri_count * sizeof(Tri)));
        const TriEx*   triex = has_triex
                               ? reinterpret_cast<const TriEx*>(r.take(tri_count * sizeof(TriEx)))
                               : nullptr;
        const BVHNode* nodes  = reinterpret_cast<const BVHNode*>(r.take(nodes_used * sizeof(BVHNode)));
        const uint*    triIdx = reinterpret_cast<const uint*>(r.take(tri_count * sizeof(uint)));
        if (!r.ok) return false;
        handles_out[i] = blas.register_prebuilt(tris, triex, static_cast<int>(tri_count),
                                                nodes, nodes_used, triIdx, hash, ref_count);
        if (handles_out[i] == INVALID_BLAS_HANDLE) return false;
    }

    // --- Internal instances ---
    const uint32_t inst_count = r.get<uint32_t>();
    if (!r.ok) return false;
    std::vector<TLASManager::DrawInstance> insts;
    insts.reserve(inst_count);
    for (uint32_t i = 0; i < inst_count; ++i) {
        const uint32_t blas_index = r.get<uint32_t>();
        const uint32_t material   = r.get<uint32_t>();
        const uint8_t* tf         = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        if (blas_index >= blas_count) return false;
        TLASManager::DrawInstance di;
        di.blas_handle = handles_out[blas_index];
        di.material_id = material;
        std::memcpy(di.transform.m, tf, 16 * sizeof(float));
        insts.push_back(di);
    }

    // --- Child instances (passive — returned to caller) ---
    const uint32_t child_count = r.get<uint32_t>();
    if (!r.ok) return false;
    children_out.reserve(child_count);
    for (uint32_t i = 0; i < child_count; ++i) {
        ChildInstance ci{};
        ci.child_resolved_hash = r.get<uint64_t>();
        const uint8_t* tf = r.take(16 * sizeof(float));
        if (!r.ok) return false;
        std::memcpy(ci.transform, tf, 16 * sizeof(float));
        children_out.push_back(ci);
    }

    // --- LOD levels (passive — returned to caller) ---
    const uint32_t level_count = r.get<uint32_t>();
    if (!r.ok) return false;
    lods_out.reserve(level_count);
    for (uint32_t i = 0; i < level_count; ++i) {
        LodLevel lvl;
        lvl.screen_size_threshold = r.get<float>();
        const uint32_t idx_count  = r.get<uint32_t>();
        if (!r.ok) return false;
        lvl.blas_indices.reserve(idx_count);
        for (uint32_t j = 0; j < idx_count; ++j) {
            const uint32_t idx = r.get<uint32_t>();
            if (!r.ok) return false;
            if (idx >= blas_count) return false; // dangling LOD index: regenerate
            lvl.blas_indices.push_back(idx);
        }
        lods_out.push_back(std::move(lvl));
    }
    if (!r.ok) return false;

    if (!insts.empty()) {
        tlas.draw_batch(insts);
        tlas.build(blas);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool replace_file_atomic(const std::string& source_path,
                         const std::string& target_path) {
#ifdef _WIN32
    return MoveFileExA(source_path.c_str(), target_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(source_path.c_str(), target_path.c_str()) == 0;
#endif
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
             uint64_t resolved_hash,
             const std::vector<VolumeEmitter>& emitters) {
    std::vector<uint8_t> body;
    std::unordered_map<BLASHandle, uint32_t> h2i;
    if (!append_common_body(body, blas, tlas, children, child_count, lods, h2i))
        return false;
    // --- EMIT trailer (optional; only written when non-empty) ---
    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u);  // 'EMIT'
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
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
    if (sz < 40) { std::fclose(f); return fail(PartAssetLoadFailure::Header, "invalid part header"); } // 40-byte v2 header
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

    // --- EMIT trailer probe (EOF-tolerant) ---
    // Old .part files have no EMIT trailer; in that case r.p == r.end and we
    // return an empty emitters_out. Files with the trailer start with the 4-byte
    // tag 0x454D4954u followed by a uint32_t count and count*sizeof(VolumeEmitter)
    // raw bytes.
    if (static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        const uint8_t* saved_p = r.p;
        uint32_t tag = r.get<uint32_t>();
        if (r.ok && tag == 0x454D4954u) {
            uint32_t count = r.get<uint32_t>();
            if (r.ok && count > 0) {
                const uint8_t* data = r.take(count * sizeof(VolumeEmitter));
                if (r.ok && data) {
                    emitters_out.resize(count);
                    std::memcpy(emitters_out.data(), data, count * sizeof(VolumeEmitter));
                }
            }
        } else {
            // Not an EMIT tag; rewind (no trailer present).
            r.p = saved_p;
        }
    }

    return true;
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
    // v8 body = v7 layout plus the optional EMIT trailer.
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

    // --- EMIT trailer (v8, optional; only written when non-empty) ---
    if (!emitters.empty()) {
        put<uint32_t>(body, 0x454D4954u);  // 'EMIT'
        put<uint32_t>(body, static_cast<uint32_t>(emitters.size()));
        put_bytes(body, emitters.data(), emitters.size() * sizeof(VolumeEmitter));
    }

    return write_file_atomic(path, kFormatVersionFlat, resolved_hash, body);
}

bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  const std::vector<FlatInstanceRef>& instance_refs,
                  uint64_t resolved_hash) {
    return save_flat_v3(path, blas, tlas, clusters, instance_refs, resolved_hash,
                        std::vector<VolumeEmitter>{});
}

bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  uint64_t resolved_hash) {
    return save_flat_v3(path, blas, tlas, clusters,
                        std::vector<FlatInstanceRef>{}, resolved_hash);
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
    if (!r.ok) return false;

    // --- EMIT trailer probe (v8, EOF-tolerant) ---
    // Flat artifacts written before v8 have no EMIT trailer; r.p == r.end and
    // emitters_out stays empty. v8 artifacts append the optional EMIT block
    // after the instance_refs trailer (same tag+count+raw format as save_v2).
    if (static_cast<size_t>(r.end - r.p) >= sizeof(uint32_t)) {
        const uint8_t* saved_p = r.p;
        uint32_t tag = r.get<uint32_t>();
        if (r.ok && tag == 0x454D4954u) {
            uint32_t count = r.get<uint32_t>();
            if (r.ok && count > 0) {
                const uint8_t* data = r.take(count * sizeof(VolumeEmitter));
                if (r.ok && data) {
                    emitters_out.resize(count);
                    std::memcpy(emitters_out.data(), data, count * sizeof(VolumeEmitter));
                }
            }
        } else {
            r.p = saved_p;  // rewind; no EMIT trailer present
        }
    }

    return true;
}

bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out,
                  std::vector<FlatInstanceRef>& instance_refs_out) {
    std::vector<VolumeEmitter> emitters_ignored;
    return load_flat_v3(path, expected_resolved_hash, blas, tlas, clusters_out,
                        instance_refs_out, emitters_ignored);
}

bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out) {
    std::vector<FlatInstanceRef> refs_ignored;
    return load_flat_v3(path, expected_resolved_hash, blas, tlas, clusters_out,
                        refs_ignored);
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
