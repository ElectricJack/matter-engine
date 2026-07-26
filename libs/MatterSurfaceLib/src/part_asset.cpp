#include "../include/part_asset.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <sys/stat.h>

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
        if (p + sizeof(T) > end) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T)); p += sizeof(T);
        return v;
    }
    const uint8_t* take(size_t n) {
        if (p + n > end) { ok = false; return nullptr; }
        const uint8_t* r = p; p += n; return r;
    }
};
} // namespace

namespace part_asset {

uint64_t fnv1a64(const void* data, size_t len) {
    uint64_t h = 1469598103934665603ull;            // FNV offset basis
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t compute_param_hash(const PartGenParams& p) {
    return fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("parts/") + buf + ".part";
}

bool save(const std::string& path, const BLASManager& blas,
          const TLASManager& tlas, uint64_t param_hash) {
    std::vector<uint8_t> body;

    // --- Materials ---
    const uint32_t mcount = static_cast<uint32_t>(MaterialRegistryCount());
    put<uint32_t>(body, mcount);
    for (uint32_t i = 0; i < mcount; ++i)
        put_bytes(body, MaterialRegistryGet(static_cast<int>(i)), sizeof(MaterialDef));

    // --- BLAS table (index == position in entries_) ---
    const auto& entries = blas.get_entries();
    put<uint32_t>(body, static_cast<uint32_t>(entries.size()));
    std::unordered_map<BLASHandle, uint32_t> handle_to_index;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        handle_to_index[e->handle] = i;
        const uint32_t tri_count  = static_cast<uint32_t>(e->mesh->triCount);
        const uint32_t nodes_used = e->bvh->nodesUsed;
        const uint32_t has_triex  = e->mesh->triEx ? 1u : 0u;
        put<uint32_t>(body, e->hash);
        put<uint32_t>(body, e->ref_count);
        put<uint32_t>(body, tri_count);
        put<uint32_t>(body, nodes_used);
        put<uint32_t>(body, has_triex);
        put_bytes(body, e->triangles.data(), tri_count * sizeof(Tri));
        if (has_triex) put_bytes(body, e->mesh->triEx, tri_count * sizeof(TriEx));
        put_bytes(body, e->bvh->bvhNode, nodes_used * sizeof(BVHNode));
        put_bytes(body, e->bvh->triIdx,  tri_count  * sizeof(uint));
    }

    // --- Instances ---
    const auto& recs = tlas.get_draw_records();
    put<uint32_t>(body, static_cast<uint32_t>(recs.size()));
    for (const auto& r : recs) {
        auto it = handle_to_index.find(r.blas_handle);
        if (it == handle_to_index.end()) return false; // dangling handle: refuse to write a corrupt cache
        const uint32_t blas_index = it->second;
        put<uint32_t>(body, blas_index);
        put<uint32_t>(body, r.material_id);
        put_bytes(body, r.transform.m, 16 * sizeof(float));
    }

    // --- Header (36 bytes) ---
    const uint64_t content_hash = fnv1a64(body.data(), body.size());
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, kFormatVersion);
    put<uint64_t>(head, param_hash);
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(Tri)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(TriEx)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(BVHNode)));
    put<uint64_t>(head, content_hash);

    // --- Atomic write ---
    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
              std::fwrite(body.data(), 1, body.size(), f) == body.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool load(const std::string& path, uint64_t expected_hash,
          BLASManager& blas, TLASManager& tlas) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 36) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data() + buf.size() };

    // --- Header + validation ---
    const uint32_t magic    = r.get<uint32_t>();
    const uint32_t version  = r.get<uint32_t>();
    const uint64_t phash    = r.get<uint64_t>();
    const uint32_t s_tri    = r.get<uint32_t>();
    const uint32_t s_triex  = r.get<uint32_t>();
    const uint32_t s_node   = r.get<uint32_t>();
    const uint64_t content  = r.get<uint64_t>();
    if (!r.ok) return false;
    if (magic != kMagic)           return false;
    if (version != kFormatVersion) return false;
    if (s_tri   != sizeof(Tri))    return false;
    if (s_triex != sizeof(TriEx))  return false;
    if (s_node  != sizeof(BVHNode)) return false;
    if (phash   != expected_hash)  return false;
    if (fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content) return false;

    // --- Materials (validate against the live code-defined registry) ---
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
    std::vector<BLASHandle> handles(blas_count, INVALID_BLAS_HANDLE);
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
        handles[i] = blas.register_prebuilt(tris, triex, static_cast<int>(tri_count),
                                            nodes, nodes_used, triIdx, hash, ref_count);
        if (handles[i] == INVALID_BLAS_HANDLE) return false; // bad/zero geometry: regenerate
    }

    // --- Instances ---
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
        di.blas_handle = handles[blas_index];
        di.material_id = material;
        std::memcpy(di.transform.m, tf, 16 * sizeof(float));
        insts.push_back(di);
    }

    if (!insts.empty()) {
        tlas.draw_batch(insts);
        tlas.build(blas);
    }
    return true;
}

} // namespace part_asset
