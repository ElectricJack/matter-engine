#include "render/lod_trace.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace viewer::lod_trace {
namespace {

// Key order IS the canonical emission order: std::map<pair<token, cluster>>
// sorts by (instance_token, cluster_index), which is exactly the ordering the
// design requires before any diff, because the GPU hands out transform slots
// with an atomicAdd and their order races between otherwise identical runs.
using Key = std::pair<uint32_t, uint32_t>;
using Selection = std::map<Key, uint32_t>;

struct Census {
    uint64_t pairs = 0;
    uint64_t tokens = 0;
    uint64_t clusters = 0;
    bool operator==(const Census& other) const {
        return pairs == other.pairs && tokens == other.tokens &&
               clusters == other.clusters;
    }
};

struct State {
    bool latched = false;
    bool active = false;
    bool capture = true;
    bool closed = false;
    std::FILE* out = nullptr;
    Selection previous;
    Census last_census;
    bool census_emitted = false;
    uint64_t frames = 0;
    uint64_t events = 0;
    uint64_t max_pairs = 0;
    uint64_t zero_tokens = 0;
    uint64_t duplicate_keys = 0;
    bool zero_token_reported = false;
    uint64_t label = 0;
    bool label_set = false;
};

State& state() {
    static State value;
    return value;
}

void latch() {
    State& s = state();
    if (s.latched) return;
    s.latched = true;
    const char* path = std::getenv("MATTER_LOD_TRACE");
    if (path == nullptr || path[0] == '\0') return;
    s.out = std::fopen(path, "wb");
    if (s.out == nullptr) {
        std::fprintf(stderr, "[lod-trace] cannot open %s for writing\n", path);
        return;
    }
    s.active = true;
    // A version line so a comparator can refuse a stream it does not
    // understand. Everything after the first space is free-form.
    std::fprintf(s.out, "V 1 lod-trace\n");
    std::fprintf(stderr, "[lod-trace] recording to %s\n", path);
}

const char* rung_text(char (&buffer)[16], bool present, uint32_t lod) {
    if (!present) return "-";
    std::snprintf(buffer, sizeof(buffer), "%u", lod);
    return buffer;
}

}  // namespace

bool enabled() {
    latch();
    return state().active;
}

void set_capture_enabled(bool value) { state().capture = value; }

bool capture_enabled() { return state().capture; }

void set_frame_label(uint64_t label) {
    State& s = state();
    s.label = label;
    s.label_set = true;
}

uint64_t frame_label(uint64_t fallback) {
    const State& s = state();
    return s.label_set ? s.label : fallback;
}

void submit_frame(uint64_t label, std::vector<Entry> entries) {
    if (!enabled()) return;
    State& s = state();
    if (!s.capture || s.closed) return;

    Selection current;
    std::set<uint32_t> tokens;
    std::set<uint32_t> clusters;
    for (const Entry& entry : entries) {
        if (entry.instance_token == 0) {
            // The token falls back to source_index + 1 upstream, so a zero here
            // means the trace key is not an identity at all. Loud, once, and
            // marked in the stream so the comparator fails the run rather than
            // reporting a beautifully stable diff of meaningless keys.
            ++s.zero_tokens;
            if (!s.zero_token_reported) {
                s.zero_token_reported = true;
                std::fprintf(stderr,
                             "[lod-trace] FATAL: instance_token == 0 for "
                             "cluster %u lod %u; the trace key is not an "
                             "instance identity\n",
                             entry.cluster_index, entry.lod);
                std::fprintf(s.out, "! zero-instance-token cluster=%u lod=%u\n",
                             entry.cluster_index, entry.lod);
            }
            continue;
        }
        const Key key{entry.instance_token, entry.cluster_index};
        const auto inserted = current.emplace(key, entry.lod);
        if (!inserted.second) {
            // Two draws for one (instance, cluster) pair. Impossible per the
            // cull dispatch's bucket arithmetic, so it means the token is not
            // unique across instances — same failure mode as a zero token.
            ++s.duplicate_keys;
            if (inserted.first->second != entry.lod) {
                std::fprintf(s.out,
                             "! duplicate-key token=%u cluster=%u lod=%u/%u\n",
                             entry.instance_token, entry.cluster_index,
                             inserted.first->second, entry.lod);
            }
            continue;
        }
        tokens.insert(entry.instance_token);
        clusters.insert(entry.cluster_index);
    }

    ++s.frames;
    const Census census{current.size(), tokens.size(), clusters.size()};
    if (census.pairs > s.max_pairs) s.max_pairs = census.pairs;
    if (!s.census_emitted || !(census == s.last_census)) {
        s.census_emitted = true;
        s.last_census = census;
        // The anti-vacuity record: pairs is the number of (instance, cluster)
        // draws the GPU actually emitted this frame.
        std::fprintf(s.out, "C %llu %llu %llu #f%llu\n",
                     (unsigned long long)census.pairs,
                     (unsigned long long)census.tokens,
                     (unsigned long long)census.clusters,
                     (unsigned long long)label);
    }

    // Ordered merge of two sorted maps: every key present in either side is
    // visited once, in (token, cluster) order, so the event sequence is
    // canonical regardless of GPU slot assignment.
    auto old_it = s.previous.begin();
    auto new_it = current.begin();
    char from_buffer[16];
    char to_buffer[16];
    while (old_it != s.previous.end() || new_it != current.end()) {
        const bool have_old = old_it != s.previous.end();
        const bool have_new = new_it != current.end();
        bool present_old = false;
        bool present_new = false;
        Key key{};
        uint32_t old_lod = 0;
        uint32_t new_lod = 0;
        if (have_old && (!have_new || old_it->first < new_it->first)) {
            key = old_it->first;
            old_lod = old_it->second;
            present_old = true;
            ++old_it;
        } else if (have_new && (!have_old || new_it->first < old_it->first)) {
            key = new_it->first;
            new_lod = new_it->second;
            present_new = true;
            ++new_it;
        } else {
            key = old_it->first;
            old_lod = old_it->second;
            new_lod = new_it->second;
            present_old = true;
            present_new = true;
            ++old_it;
            ++new_it;
        }
        if (present_old && present_new && old_lod == new_lod) continue;
        ++s.events;
        std::fprintf(s.out, "E %u %u %s %s #f%llu\n", key.first, key.second,
                     rung_text(from_buffer, present_old, old_lod),
                     rung_text(to_buffer, present_new, new_lod),
                     (unsigned long long)label);
    }
    s.previous = std::move(current);
    std::fflush(s.out);
}

void close() {
    State& s = state();
    if (!s.active || s.closed) return;
    s.closed = true;
    // Frame COUNT is deliberately absent from the summary: the number of frames
    // a run renders is timing-dependent even warm, and the gate asserts switch
    // order, not frame index. Everything here is a function of the selection.
    std::fprintf(s.out,
                 "S events=%llu max_pairs=%llu zero_tokens=%llu "
                 "duplicate_keys=%llu\n",
                 (unsigned long long)s.events,
                 (unsigned long long)s.max_pairs,
                 (unsigned long long)s.zero_tokens,
                 (unsigned long long)s.duplicate_keys);
    std::fprintf(stderr,
                 "[lod-trace] %llu frames, %llu events, max %llu drawn pairs\n",
                 (unsigned long long)s.frames, (unsigned long long)s.events,
                 (unsigned long long)s.max_pairs);
    std::fclose(s.out);
    s.out = nullptr;
    s.active = false;
}

}  // namespace viewer::lod_trace
