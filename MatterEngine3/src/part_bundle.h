#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "version_vector.h"
#include "part_asset_v2.h"   // fnv1a64, replace_file_atomic

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

// =============================================================================
// PartBundle — one file per part, replacing the artifact zoo. (M4)
// =============================================================================
//
// A part used to scatter across six files keyed on the same resolved hash:
//
//     parts/<hash>.part          compositional body
//     parts/<hash>.flat.part     flattened per-rep ladder
//     parts/<hash>.fimp          impostor atlas          (M2.5)
//     parts/<hash>.static_lods   authored ladder plan     (W5/M3)
//     parts/<hash>.hints         flatten hints
//     parts/<hash>.lods          budget-variant sidecar
//
// plus a chart trailer inside the geometry bodies. Six files means six
// half-states a crash can leave behind, six probes to keep consistent, and six
// places for a version to fail to reach. They are now SECTIONS of one file:
//
//     parts/<hash>.bundle
//
// LAYOUT
//     header (40 bytes)
//       u32 magic 'MPBN'
//       u32 bundle_format_version
//       u64 resolved_hash
//       u64 version_digest        (matter_version::digest())
//       u32 section_count
//       u32 reserved (0)
//       u64 directory_hash        (fnv1a64 over the directory bytes)
//     directory (32 bytes per section, ASCENDING BY TAG)
//       u32 tag, u32 flags, u64 offset, u64 length, u64 content_hash
//     payloads, in directory order
//
// DETERMINISM. The directory is sorted by tag and payloads follow it in that
// order, so a bundle's bytes are a pure function of its section SET -- never
// of the order the bake happened to write them. Two cold bakes that produce
// the same payloads produce byte-identical bundles even if the flatten and the
// impostor land in a different order.
//
// A SECTION PAYLOAD IS EXACTLY WHAT THE OLD FILE CONTAINED, byte for byte.
// That is deliberate: every body serializer, every trailer grammar and every
// self-describing per-artifact header is untouched, so this milestone changes
// the CONTAINER and nothing about the content. A section therefore still
// validates its own magic, format version and resolved hash independently of
// the bundle header -- belt and braces, at a cost of 40 bytes per section.
//
// WRITES ARE MERGES. The bake writes sections at different times (bake, then
// flatten, then impostor). write_section re-reads the current bundle, replaces
// or inserts one section, and republishes the whole file through
// replace_file_atomic under a per-path mutex. A bundle whose resolved hash or
// version digest disagrees with the caller's is treated as ABSENT and
// overwritten -- that is a stale orphan, not something to merge into.
//
// NO MIGRATION PATH. There is no reader for the six old files. M4 invalidates
// every cache by construction (the version vector is folded into the part
// hash), so a bundle and the files it replaced can never contend for the same
// name. Two bakes that are not bit-identical must not share a cache identity.

namespace part_bundle {

inline constexpr uint32_t kMagic = 0x4E42504Du;  // 'MPBN' little-endian
inline constexpr uint32_t kBundleFormatVersion =
    matter_version::components::kBundleFormat;

// Section tags. Four ASCII bytes, little-endian, so a hex dump reads.
enum SectionTag : uint32_t {
    // Compositional part body -- the old `.part`. Carries the child table, the
    // top-level LOD levels, and the EMIT / CHRT / ANLK trailers.
    kSectionRep0 = 0x30504552u,  // "REP0"
    // Flattened per-rep geometry ladder -- the old `.flat.part`, including its
    // cluster table, FlatInstanceRefs and per-rung chart parameterisation.
    kSectionFlat = 0x54414C46u,  // "FLAT"
    // Impostor view atlas -- the old `.fimp`, the ladder's terminal rep.
    kSectionImpostor = 0x4F504D49u,  // "IMPO"
    // Authored `static lods` bake plan -- the old `.static_lods`.
    kSectionPlan = 0x4E414C50u,  // "PLAN"
    // Flatten hints -- the old `.hints`.
    kSectionHints = 0x544E4948u,  // "HINT"
    // Budget-LOD variant sidecar -- the old `.lods`.
    kSectionVariants = 0x53524156u,  // "VARS"
    // RESERVED for M3b's content-addressed stage outputs. Nothing writes it
    // yet; it exists so that landing stages is a kStageFormat bump rather than
    // another container change. The design deliberately does not specify the
    // payload -- only that it has a place to live.
    kSectionStages = 0x45475453u,  // "STGE"
};

// "parts/<16-hex>.bundle" — the ONE cache path a part owns.
std::string cache_path_bundle(uint64_t resolved_hash);

// Insert or replace one section, preserving every other section. Returns false
// on any I/O failure; the previous bundle is left intact on failure.
bool write_section(const std::string& bundle_path, uint64_t resolved_hash,
                   uint32_t tag, const void* data, size_t length);

// Read one section's payload. False when the bundle is missing, malformed,
// keyed on a different resolved hash or version digest, or has no such
// section. A checksum mismatch is a miss, never a crash.
bool read_section(const std::string& bundle_path, uint64_t resolved_hash,
                  uint32_t tag, std::vector<uint8_t>& out);

// Presence probe without reading the payload. Same rejection rules as above.
bool has_section(const std::string& bundle_path, uint64_t resolved_hash,
                 uint32_t tag);

// Read at most `max_bytes` from the START of a section, without reading the
// rest of the file and WITHOUT verifying the section checksum (which would
// require the whole payload). This is the cache PROBE path: part_graph's
// `cached()` asks "is this artifact's header and material schema compatible"
// once per part per bake, and slurping a multi-megabyte bundle to answer it
// would be a real cost. Reads the 40-byte bundle header plus the directory
// (bounded: at most 64 entries) and then one bounded seek+read.
//
// `full_length_out` receives the section's declared length, so a caller can
// tell "short section" from "I asked for less".
bool read_section_prefix(const std::string& bundle_path, uint64_t resolved_hash,
                         uint32_t tag, size_t max_bytes,
                         std::vector<uint8_t>& out, uint64_t& full_length_out);

// Drop one section, keeping the rest. True when the bundle no longer has it
// (including when it never did). This is what "delete the flat artifact" means
// now: a bundle is one file, so removing the file would take the compositional
// body and the impostor with it.
bool remove_section(const std::string& bundle_path, uint64_t resolved_hash,
                    uint32_t tag);

// The tags present, ascending. Empty when the bundle is missing or rejected.
// Used by cache inventories and by peek_format_version.
std::vector<uint32_t> section_tags(const std::string& bundle_path);

// ===========================================================================
// Implementation. Header-only: see the note on `inline` above.
// ===========================================================================
inline std::mutex& bundle_lock();

namespace detail {

constexpr size_t kHeaderBytes = 40;
constexpr size_t kEntryBytes  = 32;

template <class T>
void put(std::vector<uint8_t>& b, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
template <class T>
bool get(const std::vector<uint8_t>& b, size_t& at, T& out) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&out, b.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

struct Section {
    uint32_t tag = 0;
    std::vector<uint8_t> payload;
};

// One mutex per process, keyed by nothing: bundle writes are rare (a handful
// per part per bake) and the critical section is one file rewrite, so a single
// lock is simpler than a per-path map and cannot deadlock. What it must
// prevent is the read-merge-write of two DIFFERENT sections of the SAME bundle
// interleaving and losing one -- which, unlike the six-file layout, would show
// up as a silently absent impostor rather than as a re-bake.

inline void ensure_parent_dir(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return;
    const std::string parent = path.substr(0, pos);
#ifdef _WIN32
    mkdir(parent.c_str());
#else
    mkdir(parent.c_str(), 0755);
#endif
}

inline bool read_whole_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(size));
    const bool ok = out.empty() ||
                    std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// Parse a bundle into its sections. `expect_hash` of 0 means "any" (used by
// section_tags, which is an inventory tool rather than a load path).
inline bool parse(const std::vector<uint8_t>& bytes, uint64_t expect_hash,
           std::vector<Section>& out) {
    out.clear();
    if (bytes.size() < kHeaderBytes) return false;
    size_t at = 0;
    uint32_t magic = 0, fmt = 0, count = 0, reserved = 0;
    uint64_t rhash = 0, digest = 0, dir_hash = 0;
    if (!get(bytes, at, magic) || !get(bytes, at, fmt) ||
        !get(bytes, at, rhash) || !get(bytes, at, digest) ||
        !get(bytes, at, count) || !get(bytes, at, reserved) ||
        !get(bytes, at, dir_hash))
        return false;
    if (magic != kMagic) return false;
    if (fmt != kBundleFormatVersion) return false;
    if (digest != matter_version::digest()) return false;
    if (expect_hash != 0 && rhash != expect_hash) return false;
    if (count > 64) return false;  // a part has a handful of sections, never 64

    const size_t dir_bytes = static_cast<size_t>(count) * kEntryBytes;
    if (bytes.size() < kHeaderBytes + dir_bytes) return false;
    if (part_asset::fnv1a64(bytes.data() + kHeaderBytes, dir_bytes) != dir_hash)
        return false;

    uint32_t previous_tag = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t tag = 0, flags = 0;
        uint64_t offset = 0, length = 0, content = 0;
        if (!get(bytes, at, tag) || !get(bytes, at, flags) ||
            !get(bytes, at, offset) || !get(bytes, at, length) ||
            !get(bytes, at, content))
            return false;
        // Strictly ascending: the writer's canonical order, and it makes a
        // duplicate tag unrepresentable rather than last-one-wins.
        if (i > 0 && tag <= previous_tag) return false;
        previous_tag = tag;
        if (offset < kHeaderBytes + dir_bytes) return false;
        if (offset > bytes.size() || length > bytes.size() - offset) return false;
        Section s;
        s.tag = tag;
        s.payload.assign(bytes.begin() + static_cast<size_t>(offset),
                         bytes.begin() + static_cast<size_t>(offset + length));
        if (part_asset::fnv1a64(s.payload.data(), s.payload.size()) != content)
            return false;  // a bad section is a cache miss, never a crash
        out.push_back(std::move(s));
    }
    return true;
}

// Serialize sections (already sorted ascending by tag) into bundle bytes.
inline std::vector<uint8_t> serialize(uint64_t resolved_hash,
                               const std::vector<Section>& sections) {
    const size_t dir_bytes = sections.size() * kEntryBytes;
    std::vector<uint8_t> dir;
    dir.reserve(dir_bytes);
    uint64_t offset = kHeaderBytes + dir_bytes;
    for (const Section& s : sections) {
        put<uint32_t>(dir, s.tag);
        put<uint32_t>(dir, 0u);  // flags
        put<uint64_t>(dir, offset);
        put<uint64_t>(dir, static_cast<uint64_t>(s.payload.size()));
        put<uint64_t>(dir, part_asset::fnv1a64(s.payload.data(), s.payload.size()));
        offset += s.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(offset));
    put<uint32_t>(out, kMagic);
    put<uint32_t>(out, kBundleFormatVersion);
    put<uint64_t>(out, resolved_hash);
    put<uint64_t>(out, matter_version::digest());
    put<uint32_t>(out, static_cast<uint32_t>(sections.size()));
    put<uint32_t>(out, 0u);  // reserved
    put<uint64_t>(out, part_asset::fnv1a64(dir.data(), dir.size()));
    out.insert(out.end(), dir.begin(), dir.end());
    for (const Section& s : sections)
        out.insert(out.end(), s.payload.begin(), s.payload.end());
    return out;
}

inline bool write_atomic(const std::string& path, const std::vector<uint8_t>& bytes) {
    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr,
                     "  part_bundle: fopen('%s','wb') failed: errno=%d (%s)\n",
                     tmp.c_str(), errno, std::strerror(errno));
        return false;
    }
    bool ok = bytes.empty() ||
              std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (ok && std::fflush(f) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = _commit(_fileno(f)) == 0;
#else
        ok = fsync(fileno(f)) == 0;
#endif
    }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    if (!part_asset::replace_file_atomic(tmp, path)) {
        std::fprintf(stderr,
                     "  part_bundle: atomic replace('%s' -> '%s') failed\n",
                     tmp.c_str(), path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace detail

using namespace detail;

inline std::mutex& bundle_lock() {
    // NB: deliberately NOT in the anonymous namespace -- an anonymous-namespace
    // static would give every translation unit its own lock, which is exactly
    // no lock at all. `inline` gives one across the program.
    static std::mutex m;
    return m;
}


inline std::string cache_path_bundle(uint64_t resolved_hash) {
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx",
                  static_cast<unsigned long long>(resolved_hash));
    return std::string("parts/") + hex + ".bundle";
}

inline bool write_section(const std::string& bundle_path, uint64_t resolved_hash,
                   uint32_t tag, const void* data, size_t length) {
    std::lock_guard<std::mutex> guard(bundle_lock());

    std::vector<Section> sections;
    std::vector<uint8_t> existing;
    if (read_whole_file(bundle_path, existing))
        (void)parse(existing, resolved_hash, sections);  // reject => start fresh

    Section incoming;
    incoming.tag = tag;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    incoming.payload.assign(p, p + length);

    auto at = std::lower_bound(
        sections.begin(), sections.end(), tag,
        [](const Section& s, uint32_t t) { return s.tag < t; });
    if (at != sections.end() && at->tag == tag)
        *at = std::move(incoming);
    else
        sections.insert(at, std::move(incoming));

    return write_atomic(bundle_path, serialize(resolved_hash, sections));
}

inline bool read_section(const std::string& bundle_path, uint64_t resolved_hash,
                  uint32_t tag, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<uint8_t> bytes;
    if (!read_whole_file(bundle_path, bytes)) return false;
    std::vector<Section> sections;
    if (!parse(bytes, resolved_hash, sections)) return false;
    for (Section& s : sections) {
        if (s.tag != tag) continue;
        out = std::move(s.payload);
        return true;
    }
    return false;
}

inline bool has_section(const std::string& bundle_path, uint64_t resolved_hash,
                 uint32_t tag) {
    std::vector<uint8_t> bytes;
    if (!read_whole_file(bundle_path, bytes)) return false;
    std::vector<Section> sections;
    if (!parse(bytes, resolved_hash, sections)) return false;
    for (const Section& s : sections)
        if (s.tag == tag) return true;
    return false;
}

inline std::vector<uint32_t> section_tags(const std::string& bundle_path) {
    std::vector<uint32_t> tags;
    std::vector<uint8_t> bytes;
    if (!read_whole_file(bundle_path, bytes)) return tags;
    std::vector<Section> sections;
    if (!parse(bytes, 0, sections)) return tags;
    for (const Section& s : sections) tags.push_back(s.tag);
    return tags;
}


inline bool read_section_prefix(const std::string& bundle_path,
                                uint64_t resolved_hash, uint32_t tag,
                                size_t max_bytes, std::vector<uint8_t>& out,
                                uint64_t& full_length_out) {
    out.clear();
    full_length_out = 0;
    FILE* f = std::fopen(bundle_path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> head(kHeaderBytes);
    if (std::fread(head.data(), 1, head.size(), f) != head.size()) {
        std::fclose(f);
        return false;
    }
    size_t at = 0;
    uint32_t magic = 0, fmt = 0, count = 0, reserved = 0;
    uint64_t rhash = 0, digest = 0, dir_hash = 0;
    const bool head_ok =
        get(head, at, magic) && get(head, at, fmt) && get(head, at, rhash) &&
        get(head, at, digest) && get(head, at, count) && get(head, at, reserved) &&
        get(head, at, dir_hash) && magic == kMagic &&
        fmt == kBundleFormatVersion && digest == matter_version::digest() &&
        rhash == resolved_hash && count <= 64;
    if (!head_ok) { std::fclose(f); return false; }

    const size_t dir_bytes = static_cast<size_t>(count) * kEntryBytes;
    std::vector<uint8_t> dir(dir_bytes);
    if (dir_bytes && std::fread(dir.data(), 1, dir_bytes, f) != dir_bytes) {
        std::fclose(f);
        return false;
    }
    if (part_asset::fnv1a64(dir.data(), dir_bytes) != dir_hash) {
        std::fclose(f);
        return false;
    }
    at = 0;
    uint64_t offset = 0, length = 0;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry_tag = 0, flags = 0;
        uint64_t off = 0, len = 0, content = 0;
        if (!get(dir, at, entry_tag) || !get(dir, at, flags) || !get(dir, at, off) ||
            !get(dir, at, len) || !get(dir, at, content)) {
            std::fclose(f);
            return false;
        }
        if (entry_tag == tag) { offset = off; length = len; found = true; }
    }
    if (!found || offset < kHeaderBytes + dir_bytes) { std::fclose(f); return false; }
    full_length_out = length;
    const size_t want = static_cast<size_t>(
        length < static_cast<uint64_t>(max_bytes) ? length : max_bytes);
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(want);
    const size_t got = want ? std::fread(out.data(), 1, want, f) : 0;
    std::fclose(f);
    out.resize(got);
    return got == want;
}

inline bool remove_section(const std::string& bundle_path, uint64_t resolved_hash,
                           uint32_t tag) {
    std::lock_guard<std::mutex> guard(bundle_lock());
    std::vector<uint8_t> existing;
    if (!read_whole_file(bundle_path, existing)) return true;  // nothing to drop
    std::vector<Section> sections;
    if (!parse(existing, resolved_hash, sections)) return true;
    const size_t before = sections.size();
    sections.erase(std::remove_if(sections.begin(), sections.end(),
                                  [tag](const Section& s) { return s.tag == tag; }),
                   sections.end());
    if (sections.size() == before) return true;
    if (sections.empty()) {
        std::remove(bundle_path.c_str());
        return true;
    }
    return write_atomic(bundle_path, serialize(resolved_hash, sections));
}

}  // namespace part_bundle
