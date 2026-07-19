// resolve_cache.cpp — resolve/manifest binary cache.
// Saves and restores the full output of LocalProvider::install_graph() +
// compose_world() so warm launches skip QuickJS script evaluation.
//
// Binary layout (little-endian, all multi-byte scalars LE):
//
//   Header (fixed):
//     u32  magic              (kResolveCacheMagic)
//     u32  format_version     (kResolveCacheVersion)
//     u64  cache_key          (compute_key() result)
//     u32  engine_bake_version (kEngineBakeVersion from tileset_gtex.h)
//
//   Payload:
//     === WorldManifest.instances ===
//     u32  instance_count
//     for each instance:
//       u32  instance_id
//       u64  part_hash
//       f32[16] transform   (row-major)
//       u32  module_len
//       u8[module_len] module  (UTF-8, no NUL)
//
//     === WorldManifest.lights ===
//     f32[3] sun_dir
//     f32[3] sun_color
//     f32[3] sky_color
//     u32    spot_count
//     for each spot:
//       f32[3] pos
//       f32[3] dir
//       f32[3] color
//       f32    range
//       f32    cos_inner
//       f32    cos_outer
//
//     === part_graph_snapshot::Snapshot ===
//     u32  node_count
//     for each node (key = module string):
//       str  module
//       str  source_path
//       str  params_json
//       u32  children_count
//       str[children_count] children
//       u32  shared_imports_count
//       str[shared_imports_count] shared_imports
//       u32  shared_source_paths_count
//       str[shared_source_paths_count] selected shared module paths
//       u64  resolved_hash
//       u8   is_root
//     (by_file and by_import are reconstructed from nodes, not stored)
//
//     === BakeInputs (ir_.bake_plan) ===
//     // String dedup table: module sources are often repeated across thousands
//     // of nodes (same schema, different params). Write a source string table
//     // once; each BakeInputs entry stores an index.
//     u32  source_table_count
//     for each source entry:
//       str  source
//     u32  bake_plan_count
//     for each bake_plan entry:
//       u64  key  (= resolved_hash)
//       u32  source_index (into source table)
//       str  module
//       // Params map (sorted keys)
//       u32  param_count
//       for each param (key, kind, value):
//         str  param_key
//         u8   kind  (0=Number, 1=Bool, 2=Str)
//         if kind==Number: f64 num
//         if kind==Bool:   u8 boolean (0/1)
//         if kind==Str:    str str
//       u32  child_hashes_count
//       u64[child_hashes_count]  child_hashes
//       u32  child_modules_count
//       str[child_modules_count] child_modules
//       u32  child_params_count
//       str[child_params_count]  child_params
//
//     === ir_.root_hashes ===
//     u32  root_hashes_count
//     u64[root_hashes_count]  root_hashes
//
// "str" in the above means: u32 len, u8[len] bytes (UTF-8, no NUL).

#include "resolve_cache.h"
#include "part_asset.h"    // fnv1a64
#include "tileset_gtex.h"  // kEngineBakeVersion

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define fs_mkdir_rc(p) _mkdir(p)
#else
#define fs_mkdir_rc(p) ::mkdir(p, 0755)
#endif

namespace resolve_cache {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// 'RC1\0' in little-endian: 0x00314352
// Version 3: retopo_by_hash section removed (schema-level retopo deleted on
// main, ea579ba). Older versions are treated as misses because
// fmt_ver != kResolveCacheVersion.
static constexpr uint32_t kResolveCacheMagic   = 0x00314352u;
static constexpr uint32_t kResolveCacheVersion = 4u;

// ---------------------------------------------------------------------------
// Low-level binary read/write helpers (little-endian)
// ---------------------------------------------------------------------------

namespace {

template <typename T>
static bool write_le(std::ofstream& f, T val) {
    static_assert(sizeof(T) <= 8, "write_le: type too wide");
    uint8_t buf[sizeof(T)];
    uint64_t v = 0;
    std::memcpy(&v, &val, sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i)
        buf[i] = (uint8_t)(v >> (i * 8));
    f.write(reinterpret_cast<const char*>(buf), (std::streamsize)sizeof(T));
    return f.good();
}

template <typename T>
static bool read_le(std::ifstream& f, T& out) {
    uint8_t buf[sizeof(T)];
    f.read(reinterpret_cast<char*>(buf), (std::streamsize)sizeof(T));
    if (!f.good()) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        v |= ((uint64_t)buf[i]) << (i * 8);
    std::memcpy(&out, &v, sizeof(T));
    return true;
}

static bool write_str(std::ofstream& f, const std::string& s) {
    uint32_t len = (uint32_t)s.size();
    if (!write_le(f, len)) return false;
    if (len > 0) {
        f.write(s.data(), (std::streamsize)len);
        if (!f.good()) return false;
    }
    return true;
}

static bool read_str(std::ifstream& f, std::string& out) {
    uint32_t len = 0;
    if (!read_le(f, len)) return false;
    if (len > 256u * 1024u * 1024u) return false;  // sanity cap 256 MiB
    out.resize(len);
    if (len > 0) {
        f.read(&out[0], (std::streamsize)len);
        if (!f.good()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Key computation helpers
// ---------------------------------------------------------------------------

// Read entire file into a buffer; returns empty on error.
static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz < 0) return {};
    f.seekg(0);
    std::vector<uint8_t> buf((size_t)sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f.good()) return {};
    return buf;
}

// Collect all regular files under `dir` recursively, returning sorted relative
// paths (relative to `dir`, with '/' separators). On any opendir failure,
// the entry is skipped (best-effort; key computation remains deterministic
// for files that ARE readable).
static void collect_files_sorted(const std::string& dir,
                                 const std::string& rel_prefix,
                                 std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    std::vector<std::string> subdirs;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name = ent->d_name;
        std::string full = dir + "/" + name;
        std::string rel  = rel_prefix.empty() ? name : rel_prefix + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            out.push_back(rel);
        } else if (S_ISDIR(st.st_mode)) {
            subdirs.push_back(name);
        }
    }
    closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& sub : subdirs)
        collect_files_sorted(dir + "/" + sub, rel_prefix.empty() ? sub : rel_prefix + "/" + sub, out);
}

static uint64_t fold_str(uint64_t h, const std::string& s) {
    // Feed length then bytes into FNV fold (incremental).
    uint32_t len = (uint32_t)s.size();
    uint8_t lbuf[4];
    for (int i = 0; i < 4; ++i) lbuf[i] = (uint8_t)(len >> (i * 8));
    const uint8_t* p = lbuf;
    for (size_t i = 0; i < 4; ++i) {
        h ^= (uint64_t)*p++;
        h *= 0x00000100000001B3ull;
    }
    const uint8_t* q = reinterpret_cast<const uint8_t*>(s.data());
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= (uint64_t)*q++;
        h *= 0x00000100000001B3ull;
    }
    return h;
}

static uint64_t fold_u32(uint64_t h, uint32_t v) {
    uint8_t buf[4];
    for (int i = 0; i < 4; ++i) buf[i] = (uint8_t)(v >> (i * 8));
    for (int i = 0; i < 4; ++i) {
        h ^= (uint64_t)buf[i];
        h *= 0x00000100000001B3ull;
    }
    return h;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

uint64_t compute_key(const std::string& world_path,
                     const std::string& root_params_json,
                     const std::string& objects_dir,
                     const std::string& project_shared_lib_dir,
                     const std::string& engine_shared_lib_dir) {
    // Start with FNV-1a offset basis.
    uint64_t h = 14695981039346656037ull;

    // 1. World JavaScript source bytes.
    {
        auto bytes = read_file_bytes(world_path);
        if (bytes.empty()) return 0;  // can't compute key without world source
        uint64_t mh = part_asset::fnv1a64(bytes.data(), bytes.size());
        h ^= mh;
        h *= 0x00000100000001B3ull;
    }

    // 2. root_params_json seed override (empty string when unset).
    h = fold_str(h, root_params_json);

    // 3. Every file under the object and both shared-library tiers. Fold an
    //    explicit tier tag so identical relative names cannot alias tiers.
    const std::pair<const char*, const std::string*> tiers[] = {
        {"objects", &objects_dir},
        {"project-shared", &project_shared_lib_dir},
        {"engine-shared", &engine_shared_lib_dir},
    };
    for (const auto& tier : tiers) {
        h = fold_str(h, tier.first);
        const std::string* dir_ptr = tier.second;
        if (dir_ptr->empty()) continue;
        std::vector<std::string> rel_files;
        collect_files_sorted(*dir_ptr, "", rel_files);
        std::sort(rel_files.begin(), rel_files.end());
        for (const auto& rel : rel_files) {
            std::string full = *dir_ptr + "/" + rel;
            auto bytes = read_file_bytes(full);
            uint64_t fh = bytes.empty() ? 0 : part_asset::fnv1a64(bytes.data(), bytes.size());
            h = fold_str(h, rel);
            h ^= fh;
            h *= 0x00000100000001B3ull;
        }
    }

    // 4. kEngineBakeVersion.
    h = fold_u32(h, tileset::kEngineBakeVersion);

    return h;
}

static std::string resolve_cache_path(const std::string& cache_root,
                                      const std::string& world_name) {
    return cache_root + "/cache/" + world_name + ".resolve";
}

bool save(const std::string& cache_root,
          const std::string& world_name,
          uint64_t           cache_key,
          const ResolveCachePayload& p) {
    const std::string cache_dir = cache_root + "/cache";
    fs_mkdir_rc(cache_dir.c_str());

    const std::string path = resolve_cache_path(cache_root, world_name);
    const std::string tmp  = path + ".tmp";

    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    // Header.
    if (!write_le(f, kResolveCacheMagic))    return false;
    if (!write_le(f, kResolveCacheVersion))  return false;
    if (!write_le(f, cache_key))             return false;
    uint32_t ebv = tileset::kEngineBakeVersion;
    if (!write_le(f, ebv))                   return false;

    // === instances ===
    {
        uint32_t cnt = (uint32_t)p.instances.size();
        if (!write_le(f, cnt)) return false;
        for (const auto& e : p.instances) {
            if (!write_le(f, e.instance_id)) return false;
            if (!write_le(f, e.part_hash))   return false;
            for (int i = 0; i < 16; ++i)
                if (!write_le(f, e.transform[i])) return false;
            if (!write_str(f, e.module)) return false;
        }
    }

    // === lights ===
    {
        const auto& l = p.lights;
        for (int i = 0; i < 3; ++i) if (!write_le(f, l.sun_dir[i]))   return false;
        for (int i = 0; i < 3; ++i) if (!write_le(f, l.sun_color[i])) return false;
        for (int i = 0; i < 3; ++i) if (!write_le(f, l.sky_color[i])) return false;
        uint32_t sc = (uint32_t)l.spots.size();
        if (!write_le(f, sc)) return false;
        for (const auto& s : l.spots) {
            for (int i = 0; i < 3; ++i) if (!write_le(f, s.pos[i]))   return false;
            for (int i = 0; i < 3; ++i) if (!write_le(f, s.dir[i]))   return false;
            for (int i = 0; i < 3; ++i) if (!write_le(f, s.color[i])) return false;
            if (!write_le(f, s.range))     return false;
            if (!write_le(f, s.cos_inner)) return false;
            if (!write_le(f, s.cos_outer)) return false;
        }
    }

    // === snapshot ===
    {
        const auto& snap = p.snapshot;
        uint32_t nc = (uint32_t)snap.nodes.size();
        if (!write_le(f, nc)) return false;
        for (const auto& kv : snap.nodes) {
            const auto& n = kv.second;
            if (!write_str(f, n.module))      return false;
            if (!write_str(f, n.source_path)) return false;
            if (!write_str(f, n.params_json)) return false;
            uint32_t cc = (uint32_t)n.children.size();
            if (!write_le(f, cc)) return false;
            for (const auto& c : n.children)
                if (!write_str(f, c)) return false;
            uint32_t sic = (uint32_t)n.shared_imports.size();
            if (!write_le(f, sic)) return false;
            for (const auto& si : n.shared_imports)
                if (!write_str(f, si)) return false;
            uint32_t sspc = (uint32_t)n.shared_source_paths.size();
            if (!write_le(f, sspc)) return false;
            for (const auto& path : n.shared_source_paths)
                if (!write_str(f, path)) return false;
            if (!write_le(f, n.resolved_hash)) return false;
            uint8_t ir = n.is_root ? 1u : 0u;
            if (!write_le(f, ir)) return false;
        }
    }

    // === bake_plan ===
    // Build source dedup table first.
    {
        // Collect unique sources in insertion order (stable across saves for same
        // world; deterministic because we iterate bake_plan in hash-key order,
        // which is not strictly deterministic across std::unordered_map, but the
        // table is referenced only by index at load time so any consistent
        // ordering is fine).
        std::vector<std::string> src_table;
        std::unordered_map<std::string, uint32_t> src_index;
        for (const auto& kv : p.bake_plan) {
            const std::string& src = kv.second.source;
            if (!src_index.count(src)) {
                src_index[src] = (uint32_t)src_table.size();
                src_table.push_back(src);
            }
        }

        uint32_t src_count = (uint32_t)src_table.size();
        if (!write_le(f, src_count)) return false;
        for (const auto& s : src_table)
            if (!write_str(f, s)) return false;

        uint32_t bp_count = (uint32_t)p.bake_plan.size();
        if (!write_le(f, bp_count)) return false;

        for (const auto& kv : p.bake_plan) {
            uint64_t resolved_hash = kv.first;
            const part_graph::BakeInputs& bi = kv.second;

            if (!write_le(f, resolved_hash)) return false;

            uint32_t si = src_index.at(bi.source);
            if (!write_le(f, si)) return false;

            if (!write_str(f, bi.module)) return false;

            // Params map (std::map<string,ParamValue> — already sorted by key).
            uint32_t pc = (uint32_t)bi.params.size();
            if (!write_le(f, pc)) return false;
            for (const auto& pk : bi.params) {
                if (!write_str(f, pk.first)) return false;
                uint8_t kind = (uint8_t)pk.second.kind;
                if (!write_le(f, kind)) return false;
                switch (pk.second.kind) {
                case part_graph::ParamValue::Kind::Number: {
                    double num = pk.second.num;
                    if (!write_le(f, num)) return false;
                    break;
                }
                case part_graph::ParamValue::Kind::Bool: {
                    uint8_t bv = pk.second.boolean ? 1u : 0u;
                    if (!write_le(f, bv)) return false;
                    break;
                }
                case part_graph::ParamValue::Kind::Str:
                    if (!write_str(f, pk.second.str)) return false;
                    break;
                }
            }

            // child_hashes
            uint32_t chc = (uint32_t)bi.child_hashes.size();
            if (!write_le(f, chc)) return false;
            for (uint64_t ch : bi.child_hashes)
                if (!write_le(f, ch)) return false;

            // child_modules
            uint32_t cmc = (uint32_t)bi.child_modules.size();
            if (!write_le(f, cmc)) return false;
            for (const auto& cm : bi.child_modules)
                if (!write_str(f, cm)) return false;

            // child_params
            uint32_t cpc = (uint32_t)bi.child_params.size();
            if (!write_le(f, cpc)) return false;
            for (const auto& cp : bi.child_params)
                if (!write_str(f, cp)) return false;
        }
    }

    // === root_hashes ===
    {
        uint32_t rhc = (uint32_t)p.root_hashes.size();
        if (!write_le(f, rhc)) return false;
        for (uint64_t rh : p.root_hashes)
            if (!write_le(f, rh)) return false;
    }

    f.close();
    if (!f.good() && !f.eof()) {
        std::remove(tmp.c_str());
        return false;
    }

    // Atomic rename.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool load(const std::string& cache_root,
          const std::string& world_name,
          uint64_t           expected_key,
          ResolveCachePayload& out) {
    const std::string path = resolve_cache_path(cache_root, world_name);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Header validation.
    uint32_t magic = 0, fmt_ver = 0, ebv = 0;
    uint64_t stored_key = 0;
    if (!read_le(f, magic))      return false;
    if (magic != kResolveCacheMagic) return false;
    if (!read_le(f, fmt_ver))    return false;
    if (fmt_ver != kResolveCacheVersion) return false;
    if (!read_le(f, stored_key)) return false;
    if (stored_key != expected_key) return false;
    if (!read_le(f, ebv))        return false;
    if (ebv != tileset::kEngineBakeVersion) return false;

    // === instances ===
    {
        uint32_t cnt = 0;
        if (!read_le(f, cnt)) return false;
        out.instances.resize(cnt);
        for (uint32_t i = 0; i < cnt; ++i) {
            auto& e = out.instances[i];
            if (!read_le(f, e.instance_id)) return false;
            if (!read_le(f, e.part_hash))   return false;
            for (int j = 0; j < 16; ++j)
                if (!read_le(f, e.transform[j])) return false;
            if (!read_str(f, e.module)) return false;
        }
    }

    // === lights ===
    {
        auto& l = out.lights;
        for (int i = 0; i < 3; ++i) if (!read_le(f, l.sun_dir[i]))   return false;
        for (int i = 0; i < 3; ++i) if (!read_le(f, l.sun_color[i])) return false;
        for (int i = 0; i < 3; ++i) if (!read_le(f, l.sky_color[i])) return false;
        uint32_t sc = 0;
        if (!read_le(f, sc)) return false;
        l.spots.resize(sc);
        for (uint32_t i = 0; i < sc; ++i) {
            auto& s = l.spots[i];
            for (int j = 0; j < 3; ++j) if (!read_le(f, s.pos[j]))   return false;
            for (int j = 0; j < 3; ++j) if (!read_le(f, s.dir[j]))   return false;
            for (int j = 0; j < 3; ++j) if (!read_le(f, s.color[j])) return false;
            if (!read_le(f, s.range))     return false;
            if (!read_le(f, s.cos_inner)) return false;
            if (!read_le(f, s.cos_outer)) return false;
        }
    }

    // === snapshot ===
    {
        auto& snap = out.snapshot;
        snap.nodes.clear();
        snap.by_file.clear();
        snap.by_import.clear();
        uint32_t nc = 0;
        if (!read_le(f, nc)) return false;
        for (uint32_t i = 0; i < nc; ++i) {
            part_graph_snapshot::Node n;
            if (!read_str(f, n.module))      return false;
            if (!read_str(f, n.source_path)) return false;
            if (!read_str(f, n.params_json)) return false;
            uint32_t cc = 0;
            if (!read_le(f, cc)) return false;
            n.children.resize(cc);
            for (uint32_t j = 0; j < cc; ++j)
                if (!read_str(f, n.children[j])) return false;
            uint32_t sic = 0;
            if (!read_le(f, sic)) return false;
            n.shared_imports.resize(sic);
            for (uint32_t j = 0; j < sic; ++j)
                if (!read_str(f, n.shared_imports[j])) return false;
            uint32_t sspc = 0;
            if (!read_le(f, sspc)) return false;
            n.shared_source_paths.resize(sspc);
            for (uint32_t j = 0; j < sspc; ++j)
                if (!read_str(f, n.shared_source_paths[j])) return false;
            if (!read_le(f, n.resolved_hash)) return false;
            uint8_t ir = 0;
            if (!read_le(f, ir)) return false;
            n.is_root = (ir != 0);

            // Reconstruct by_file and by_import indices.
            if (!n.source_path.empty())
                snap.by_file[n.source_path].push_back(n.module);
            for (const auto& path : n.shared_source_paths)
                snap.by_file[path].push_back(n.module);
            for (const auto& si : n.shared_imports)
                snap.by_import[si].push_back(n.module);

            snap.nodes[n.module] = std::move(n);
        }
    }

    // === bake_plan ===
    {
        out.bake_plan.clear();
        // Read source dedup table.
        uint32_t src_count = 0;
        if (!read_le(f, src_count)) return false;
        if (src_count > 1024u * 1024u) return false;  // sanity
        std::vector<std::string> src_table(src_count);
        for (uint32_t i = 0; i < src_count; ++i)
            if (!read_str(f, src_table[i])) return false;

        uint32_t bp_count = 0;
        if (!read_le(f, bp_count)) return false;
        if (bp_count > 1024u * 1024u) return false;  // sanity

        for (uint32_t i = 0; i < bp_count; ++i) {
            uint64_t resolved_hash = 0;
            if (!read_le(f, resolved_hash)) return false;

            part_graph::BakeInputs bi;

            uint32_t si = 0;
            if (!read_le(f, si)) return false;
            if (si >= src_count) return false;
            bi.source = src_table[si];

            if (!read_str(f, bi.module)) return false;

            // Params.
            uint32_t pc = 0;
            if (!read_le(f, pc)) return false;
            if (pc > 65536u) return false;  // sanity
            for (uint32_t j = 0; j < pc; ++j) {
                std::string pk;
                if (!read_str(f, pk)) return false;
                uint8_t kind = 0;
                if (!read_le(f, kind)) return false;
                part_graph::ParamValue pv;
                switch (kind) {
                case 0: { // Number
                    pv.kind = part_graph::ParamValue::Kind::Number;
                    if (!read_le(f, pv.num)) return false;
                    break;
                }
                case 1: { // Bool
                    pv.kind = part_graph::ParamValue::Kind::Bool;
                    uint8_t bv = 0;
                    if (!read_le(f, bv)) return false;
                    pv.boolean = (bv != 0);
                    break;
                }
                case 2: { // Str
                    pv.kind = part_graph::ParamValue::Kind::Str;
                    if (!read_str(f, pv.str)) return false;
                    break;
                }
                default:
                    return false;
                }
                bi.params[pk] = pv;
            }

            // child_hashes
            uint32_t chc = 0;
            if (!read_le(f, chc)) return false;
            if (chc > 65536u) return false;
            bi.child_hashes.resize(chc);
            for (uint32_t j = 0; j < chc; ++j)
                if (!read_le(f, bi.child_hashes[j])) return false;

            // child_modules
            uint32_t cmc = 0;
            if (!read_le(f, cmc)) return false;
            if (cmc > 65536u) return false;
            bi.child_modules.resize(cmc);
            for (uint32_t j = 0; j < cmc; ++j)
                if (!read_str(f, bi.child_modules[j])) return false;

            // child_params
            uint32_t cpc = 0;
            if (!read_le(f, cpc)) return false;
            if (cpc > 65536u) return false;
            bi.child_params.resize(cpc);
            for (uint32_t j = 0; j < cpc; ++j)
                if (!read_str(f, bi.child_params[j])) return false;

            out.bake_plan[resolved_hash] = std::move(bi);
        }
    }

    // === root_hashes ===
    {
        out.root_hashes.clear();
        uint32_t rhc = 0;
        if (!read_le(f, rhc)) return false;
        if (rhc > 65536u) return false;  // sanity
        out.root_hashes.resize(rhc);
        for (uint32_t i = 0; i < rhc; ++i)
            if (!read_le(f, out.root_hashes[i])) return false;
    }

    // Confirm we're at EOF (detect truncation of a valid header + too-short payload).
    {
        char sentinel;
        f.read(&sentinel, 1);
        if (!f.eof()) return false;   // trailing bytes = corruption / partial overwrite
    }

    return true;
}

} // namespace resolve_cache
