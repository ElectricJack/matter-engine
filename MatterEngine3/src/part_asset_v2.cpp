#include "part_asset_v2.h"
#include "matter/lod_contract.h"
#include "version_vector.h"   // M4: the one fold site for cache keys
#include "part_bundle.h"      // M4: the one file a part owns
// The FLAT section IS part_flatten's output, so its identity is part_flatten's
// business: active_ladder_shape_digest() names the ladder shape this process
// bakes. Header-only (inline), so this costs no link dependency from the
// serializer onto the baker.
#include "part_flatten.h"     // active_ladder_shape_digest (flat identity)

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

// Pin the consumed Tri layout the same way. Tri is ALIGN(64) with four
// union{float3; __m128} slots at 0/16/32/48, and put_canonical_tris below
// zero-fills the destination and copies sizeof(float3) == 12 bytes into each
// slot. That is the whole of the written data only while the slots really are
// 16 apart and float3 really is 12 bytes; if tri.h or precomp.h reshapes
// either, these fire rather than letting the zero-fill silently eat vertex
// bytes on the way to disk.
static_assert(sizeof(Tri) == 64, "Tri layout changed: canonical-triangle writer assumptions broken");
static_assert(sizeof(float3) == 12, "float3 is no longer 12 bytes: Tri union padding assumption stale");
static_assert(offsetof(Tri, vertex0)  ==  0, "Tri slot 0 moved");
static_assert(offsetof(Tri, vertex1)  == 16, "Tri slot 1 moved");
static_assert(offsetof(Tri, vertex2)  == 32, "Tri slot 2 moved");
static_assert(offsetof(Tri, centroid) == 48, "Tri slot 3 moved");

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

// Append `count` triangles with their union padding ZEROED.
//
// Tri's four slots are union{float3 vertexN; __m128 vN} — 16 bytes each, of
// which a float3 assignment writes 12. Bytes 12-15 of every slot are therefore
// whatever the producing allocation happened to hold, and nothing in the engine
// ever writes them: meshers, transforms and simplifiers all assign the float3
// member. Nothing READS them either — BLASManager::calculate_hash folds the
// named floats one at a time and impostor::depicts_hash_add_cluster copies the
// nine vertex components out, both for exactly this reason.
//
// But writing the array verbatim copied that garbage into the artifact: 16 of
// every 64 triangle bytes, plus every checksum computed downstream of them. Two
// processes baking the SAME geometry under the SAME settings produced bundles
// differing in thousands of bytes, which destroys the property the
// content-addressed cache is built on (same settings -> same bytes) and
// silently defeats any cross-process byte-diff used to verify a bake change.
// The in-suite gates never saw it because they save twice from one in-memory
// BLAS inside one process, so the same garbage went down both times; the gate
// that can actually fail is part_flatten_tests' two-child-process compare.
//
// This is the same defect and the same fix as the TriEx staging in
// append_common_body — where the comment used to assert Tri had no such gap.
// It does; four of them.
void put_canonical_tris(std::vector<uint8_t>& b, const Tri* tris, size_t count) {
    const size_t base = b.size();
    b.resize(base + count * sizeof(Tri), 0u);  // padding is zero by construction
    uint8_t* dst = b.data() + base;
    for (size_t i = 0; i < count; ++i, dst += sizeof(Tri)) {
        std::memcpy(dst + offsetof(Tri, vertex0),  &tris[i].vertex0,  sizeof(float3));
        std::memcpy(dst + offsetof(Tri, vertex1),  &tris[i].vertex1,  sizeof(float3));
        std::memcpy(dst + offsetof(Tri, vertex2),  &tris[i].vertex2,  sizeof(float3));
        std::memcpy(dst + offsetof(Tri, centroid), &tris[i].centroid, sizeof(float3));
    }
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
    // M4: the version vector, folded here and nowhere else. This hash NAMES
    // every artifact a part owns on disk, so folding the vector into it is
    // what makes a version bump re-BAKE rather than merely re-resolve — the
    // hole that let stale geometry be served under new rules
    // (version_vector.h, docs/lod-vt-redesign-2026-08-04.md §9.3).
    return matter_version::fold(h);
}

// M4: every one of these used to name a different file. They now all name the
// part's single bundle -- the KIND is a section tag inside it, chosen by the
// save/load function itself rather than by the caller's filename. Call sites
// are unchanged on purpose: which artifact a caller wants was never expressed
// by the extension, only by the function it called.
std::string cache_path_resolved(uint64_t resolved_hash) {
    return part_bundle::cache_path_bundle(resolved_hash);
}

std::string cache_path_flat(uint64_t resolved_hash) {
    return part_bundle::cache_path_bundle(resolved_hash);
}

std::string cache_path_lods(uint64_t resolved_hash) {
    return part_bundle::cache_path_bundle(resolved_hash);
}

std::string cache_path_static_lods(uint64_t resolved_hash) {
    return part_bundle::cache_path_bundle(resolved_hash);
}

std::string cache_path_hints(uint64_t resolved_hash) {
    return part_bundle::cache_path_bundle(resolved_hash);
}

// ---------------------------------------------------------------------------
// The three text sidecars, now bundle SECTIONS.
//
// M4 added a resolved_hash parameter to each. The hash was always implicit in
// the path -- but a bundle section has to state which part it belongs to, and
// scraping 16 hex digits back out of a filename is exactly the implicit
// coupling this milestone is removing. Every caller already had the hash in
// scope; passing it costs nothing and makes the identity checkable.
//
// The payload bytes are the same text these files always held, so the
// grammars (and their round-trip tests) are unchanged.
// ---------------------------------------------------------------------------

namespace {
bool read_text_section(const std::string& path, uint64_t resolved_hash,
                       uint32_t tag, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!part_bundle::read_section(path, resolved_hash, tag, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}
bool write_text_section(const std::string& path, uint64_t resolved_hash,
                        uint32_t tag, const std::string& text) {
    return part_bundle::write_section(path, resolved_hash, tag,
                                      text.data(), text.size());
}
}  // namespace

bool save_lod_sidecar(const std::string& path, uint64_t resolved_hash,
                      const LodVariants& v) {
    if (v.budgets.size() != v.hashes.size() || v.hashes.empty()) return false;
    std::ostringstream out;
    out << v.anchor_size << "\n";
    for (size_t i = 0; i < v.budgets.size(); ++i) {
        char hex[17];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)v.hashes[i]);
        out << v.budgets[i] << " " << hex << "\n";
    }
    return write_text_section(path, resolved_hash,
                              part_bundle::kSectionVariants, out.str());
}

bool load_lod_sidecar(const std::string& path, uint64_t resolved_hash,
                      LodVariants& out) {
    std::string text;
    if (!read_text_section(path, resolved_hash, part_bundle::kSectionVariants, text))
        return false;
    std::istringstream in(text);
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

// Line format, one per authored level:
//   <hash16> <mask8> <at> <gen>
// `at` is the authored switch-in distance in metres, or -1 when the level
// declared none. `gen` is "-" for no generator, else the generator name and
// its canonical params joined by '|' (canonical JSON never contains a space,
// so the record stays whitespace-delimited and `>>`-readable).
//
// A two-token line is the pre-M3 form and still loads, with at = -1 and no
// generator -- which is exactly "this plan does not drive the ladder".
//
// One OPTIONAL first line, `!noimp`, records the per-part impostor opt-out
// (StaticLodPlan::no_impostor, kRepresentation 1->2). It leads the section so a
// reader can classify the plan before touching the level records, it begins
// with '!' (a level line always begins with a 16-hex hash, so the two grammars
// never collide), and a plan that carries ONLY this line — an opt-out on a part
// with no other authored levels — is valid and has zero level records.
bool save_static_lod_plan(const std::string& path, uint64_t resolved_hash,
                          const StaticLodPlan& plan) {
    const size_t n = plan.level_hashes.size();
    if (plan.level_exclude_masks.size() != n) return false;
    if (!plan.level_at.empty()  && plan.level_at.size()  != n) return false;
    if (!plan.level_gen.empty() && plan.level_gen.size() != n) return false;
    std::ostringstream out;
    if (plan.no_impostor) out << "!noimp\n";
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
    return write_text_section(path, resolved_hash,
                              part_bundle::kSectionPlan, out.str());
}

bool load_static_lod_plan(const std::string& path, uint64_t resolved_hash,
                          StaticLodPlan& out) {
    std::string text;
    if (!read_text_section(path, resolved_hash, part_bundle::kSectionPlan, text))
        return false;
    std::istringstream in(text);
    StaticLodPlan plan;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line == "!noimp") { plan.no_impostor = true; continue; }
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
    // A plan with no level records is valid ONLY when it carries the opt-out
    // flag (the one thing a levelless plan can express); otherwise an empty
    // section is a corrupt/absent plan and callers should fall back.
    if (plan.level_hashes.empty() && !plan.no_impostor) return false;
    out = std::move(plan);
    return true;
}

bool save_flatten_hints(const std::string& path, uint64_t resolved_hash,
                        const FlattenHints& hints) {
    std::ostringstream out;
    for (const auto& [idx, px] : hints.child_px)
        out << idx << " " << px << "\n";
    return write_text_section(path, resolved_hash,
                              part_bundle::kSectionHints, out.str());
}

bool load_flatten_hints(const std::string& path, uint64_t resolved_hash,
                        FlattenHints& out_hints) {
    std::string text;
    if (!read_text_section(path, resolved_hash, part_bundle::kSectionHints, text))
        return false;
    std::istringstream ifs(text);
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
        // Both geometry tables go out through zeroed staging: Tri has FOUR
        // 4-byte union gaps (put_canonical_tris) and TriEx one trailing gap
        // (below). Neither is ever written by a producer, and writing either
        // verbatim makes otherwise-identical bakes byte-differ across
        // processes.
        put_canonical_tris(body, e->triangles.data(), tri_count);
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
            // touching the read-only mesher.
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
// M4: which SECTION of the bundle a body belongs to is decided by the format
// version the body was serialized under -- the same discriminator the old
// filename encoded, now stated once instead of by two path helpers that
// happened to append different suffixes.
static uint32_t bundle_tag_for_format(uint32_t format_version) {
    return format_version == kFormatVersionFlat ? part_bundle::kSectionFlat
                                                : part_bundle::kSectionRep0;
}

// LADDER-SHAPE SALT — the flat's staleness gate.
//
// The artifact header already scrambles the resolved hash with the format
// version so a body cannot be read under the wrong version. A flat needs one
// more term: the SHAPE of the ladder it holds. A knob like
// MATTER_LOD_MAX_MESH_RUNGS or MATTER_IMPOSTOR_DISTANCE changes what a bake
// would write without changing either the part hash or the format version, so
// without this term LocalProvider::ensure_part_flattened finds a "compatible"
// flat and skips the bake — and the knob does nothing, on a warm cache only.
// part_flatten.h's ladder_shape_digest carries the full reasoning.
//
// It is a SALT on the identity rather than a new header field on purpose: no
// new bytes, no new format version, and the existing bounded prefix probe
// (is_cache_artifact_header_compatible) rejects a stale flat without reading
// any more of the file than it already did. The digest of the shipped default
// shape is 0, so a default bake is byte-identical to what shipped.
//
// REP0 IS DELIBERATELY UNSALTED: a .part is the authored geometry, which no
// ladder knob touches. Only the FLAT section carries a ladder.
//
// The bundle-level address (part_bundle::write_section / read_section) keeps
// using the UNSALTED hash, so a reshaped bake OVERWRITES the stale flat in
// place instead of accumulating a second section beside it.
static uint64_t flat_identity_salt(uint32_t format_version) {
    if (format_version != kFormatVersionFlat) return 0;
    return part_flatten::active_ladder_shape_digest();
}

// Serialize the 40-byte artifact header + body and publish it as one bundle
// section. THE SECTION PAYLOAD IS EXACTLY WHAT THE STANDALONE FILE HELD: the
// header stays, so a section still validates its own magic, format version,
// struct sizes and resolved hash without trusting the bundle around it.
static bool write_file_atomic(const std::string& path,
                              uint32_t version,
                              uint64_t resolved_hash,
                              const std::vector<uint8_t>& body) {
    const uint64_t content_hash = fnv1a64(body.data(), body.size());
    std::vector<uint8_t> payload;
    payload.reserve(40 + body.size());
    put<uint32_t>(payload, kMagic);
    put<uint32_t>(payload, version);
    put<uint64_t>(payload, resolved_hash ^ static_cast<uint64_t>(version) ^
                               flat_identity_salt(version));
    put<uint32_t>(payload, static_cast<uint32_t>(sizeof(Tri)));
    put<uint32_t>(payload, static_cast<uint32_t>(sizeof(TriEx)));
    put<uint32_t>(payload, static_cast<uint32_t>(sizeof(BVHNode)));
    put<uint32_t>(payload, static_cast<uint32_t>(sizeof(ChildInstance)));
    put<uint64_t>(payload, content_hash);
    payload.insert(payload.end(), body.begin(), body.end());

    if (!part_bundle::write_section(path, resolved_hash,
                                    bundle_tag_for_format(version),
                                    payload.data(), payload.size())) {
        std::fprintf(stderr,
                     "  save_v2: bundle section write failed for '%s' "
                     "(format %u, hash %016llx)\n",
                     path.c_str(), version,
                     (unsigned long long)resolved_hash);
        return false;
    }
    return true;
}

// Read one geometry section's payload (header + body) out of the bundle.
static bool read_artifact_section(const std::string& path,
                                  uint64_t resolved_hash,
                                  uint32_t format_version,
                                  std::vector<uint8_t>& out) {
    return part_bundle::read_section(path, resolved_hash,
                                     bundle_tag_for_format(format_version), out);
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
    const uint64_t salt = flat_identity_salt(version);
    const uint64_t resolved = rhash_x ^ static_cast<uint64_t>(version) ^ salt;
    if (resolved != expected_resolved_hash) {
        // Say WHY, for a flat. Every other rejection in this function is
        // silent because it guards against a file that should never have been
        // offered; this one is routine and expected — it is what makes a knob
        // change re-bake — so it reports itself the way impostor_bake reports
        // a rejected atlas instead of drawing it. Without the line a
        // knob-driven rebake is indistinguishable from an unexplained miss.
        //
        // For a FLAT section this is UNAMBIGUOUS: the bundle layer already
        // matched the part's resolved hash in its own header (part_bundle's
        // parse / read_section_prefix both require rhash == resolved_hash)
        // before this payload was handed over. So a flat that reaches here
        // with the wrong artifact-header identity is the right part under a
        // different ladder shape — in either direction, a shaped flat found by
        // a default run or the reverse.
        if (version == kFormatVersionFlat) {
            const uint64_t on_disk =
                rhash_x ^ static_cast<uint64_t>(version) ^ expected_resolved_hash;
            std::fprintf(stderr,
                         "  part_asset: flat %016llx carries ladder shape "
                         "%016llx, this bake is %016llx; rejecting it as stale "
                         "(it will be re-flattened).\n",
                         (unsigned long long)expected_resolved_hash,
                         (unsigned long long)on_disk,
                         (unsigned long long)salt);
        }
        return 0;
    }
    content_hash_out = content;
    return version;
}

bool is_cache_artifact_header_compatible(
    const std::string& path, uint64_t expected_resolved_hash,
    uint32_t expected_format_version, CacheArtifactProbeStats* stats) {
    if (stats) *stats = CacheArtifactProbeStats{};
    // M4: the probe reads a bounded PREFIX of the bundle section, not the
    // whole file. It runs once per part per bake (part_graph's `cached()`), so
    // slurping a multi-megabyte bundle to answer a header question would be a
    // real regression -- and the CacheArtifactProbeStats assertions in
    // part_asset_v2_tests exist precisely to keep it bounded. Deliberately does
    // NOT verify the section checksum: judging the body is the loader's job,
    // and checking it here would mean reading all of it.
    const size_t material_prefix_size =
        2u * sizeof(uint32_t) +
        static_cast<size_t>(MaterialRegistryStaticCount()) * sizeof(MaterialDef);
    const size_t want = 40 + material_prefix_size;
    std::vector<uint8_t> prefix;
    uint64_t section_length = 0;
    if (!part_bundle::read_section_prefix(
            path, expected_resolved_hash,
            bundle_tag_for_format(expected_format_version), want, prefix,
            section_length))
        return false;
    if (stats) {
        stats->max_read_chunk = prefix.size();
        stats->body_bytes = prefix.size() > 40 ? prefix.size() - 40 : 0;
        stats->retained_material_bytes = stats->body_bytes;
    }
    if (prefix.size() < want) return false;
    Reader reader{prefix.data(), prefix.data() + prefix.size()};
    uint64_t content_hash_ignored = 0;
    if (!read_and_validate_header(reader, expected_resolved_hash,
                                  expected_format_version,
                                  content_hash_ignored))
        return false;
    const uint32_t material_schema = reader.get<uint32_t>();
    const uint32_t material_count = reader.get<uint32_t>();
    if (!reader.ok ||
        material_schema != MaterialRegistrySchemaVersion() ||
        material_count != static_cast<uint32_t>(MaterialRegistryStaticCount()))
        return false;
    for (uint32_t i = 0; i < material_count; ++i) {
        const uint8_t* serialized = reader.take(sizeof(MaterialDef));
        if (!reader.ok ||
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
    if (!read_artifact_section(path, expected_resolved_hash, kFormatVersionV2,
                               out.bytes) ||
        out.bytes.size() < 40)
        return fail(PartAssetLoadFailure::Header, "invalid part header");
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
    // WP-A's CHRT trailer (chart-space VT sidecar) is byte-size framed; inert
    // here -- the chart table is read by the load_v2 overload that asks for it
    // -- but it must be skipped so chart-bearing parts still load through the
    // strict suffix grammar (same rationale as the EMIT block above).
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
    // stay byte-identical (same gate discipline as EMIT above). Payload is
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

    std::vector<uint8_t> buf;
    if (!read_artifact_section(path, expected_resolved_hash, kFormatVersionV2, buf) ||
        buf.size() < 40)
        return fail(PartAssetLoadFailure::Header, "invalid part header");

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

    std::vector<uint8_t> buf;
    if (!read_artifact_section(path, expected_resolved_hash, kFormatVersionFlat, buf) ||
        buf.size() < 40)
        return false;

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

    std::vector<uint8_t> buf;
    if (!read_artifact_section(path, expected_resolved_hash, kFormatVersionFlat, buf) ||
        buf.size() < 40)
        return false;

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

// M4: every caller of this asks exactly one question -- "is there a FLATTENED
// artifact here?" -- and used to get its answer from the version field of a
// file that only existed if the answer was yes. A bundle holds the
// compositional body too, so returning "the version of whatever is at this
// path" would answer a DIFFERENT question and answer it wrongly: PartStore's
// flat-preferred load would see kFormatVersionV2, take its legacy-v2-flat
// branch, and load the compositional body AS a flat -- dropping the child
// table. (viewer_logic_tests caught exactly that.)
//
// So this reports the flat format when a FLAT section exists and 0 otherwise.
// The legacy-v2-flat branch in part_store is thereby unreachable: a v2-format
// body in the flat position was only ever a pre-Task-11 `.flat.part` file, and
// the bundle makes that unrepresentable -- the FLAT tag only ever carries
// kFormatVersionFlat bodies.
uint32_t peek_format_version(const std::string& path) {
    for (uint32_t t : part_bundle::section_tags(path))
        if (t == part_bundle::kSectionFlat) return kFormatVersionFlat;
    return 0;
}

} // namespace part_asset
