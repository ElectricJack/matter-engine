#include "retopo_blacklist.h"

#include <cinttypes>   // PRIx64 / SCNx64
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#ifndef _WIN32
#include <unistd.h>    // fsync (POSIX only; Windows viewer skips crash-safe fsync)
#endif
#include <unordered_set>

namespace matter_engine3 { namespace retopo_blacklist {

namespace {

std::unordered_set<uint64_t> g_blacklist;
std::string g_pending_path;
std::string g_success_path;
bool g_initialized = false;

// Load a journal file (one hex-uint64 per line). Silently returns empty on
// missing file. Malformed lines are skipped.
std::unordered_set<uint64_t> load_journal(const std::string& path) {
    std::unordered_set<uint64_t> out;
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return out;
    char line[64];
    while (std::fgets(line, sizeof line, fp)) {
        uint64_t h = 0;
        if (std::sscanf(line, "%" SCNx64, &h) == 1) {
            out.insert(h);
        }
    }
    std::fclose(fp);
    return out;
}

// Append one hex-uint64 line to a journal, with fflush + fsync so a crash
// before the return preserves the entry.
void append_journal(const std::string& path, uint64_t h) {
    FILE* fp = std::fopen(path.c_str(), "a");
    if (!fp) return;
    std::fprintf(fp, "%016" PRIx64 "\n", h);
    std::fflush(fp);
#ifndef _WIN32
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
#endif
    std::fclose(fp);
}

} // namespace

void init(const std::string& cache_root) {
    // Journal files live under <cache_root>/parts/ alongside the .part artifacts
    // so they move with the cache dir.
    g_pending_path = cache_root + "/parts/.retopo_pending";
    g_success_path = cache_root + "/parts/.retopo_success";

    auto pending = load_journal(g_pending_path);
    auto success = load_journal(g_success_path);

    g_blacklist.clear();
    for (uint64_t h : pending) {
        if (success.find(h) == success.end()) {
            g_blacklist.insert(h);
        }
    }
    g_initialized = true;

    if (!g_blacklist.empty()) {
        std::fprintf(stderr,
            "retopo_blacklist: loaded %zu known-crasher hash(es) from %s\n",
            g_blacklist.size(), g_pending_path.c_str());
    }
}

bool is_blacklisted(uint64_t hash) {
    return g_initialized && g_blacklist.count(hash) > 0;
}

void begin_attempt(uint64_t hash) {
    if (!g_initialized) return;
    append_journal(g_pending_path, hash);
}

void end_attempt(uint64_t hash) {
    if (!g_initialized) return;
    append_journal(g_success_path, hash);
}

uint64_t blacklist_size() {
    return g_blacklist.size();
}

void reset_for_tests() {
    g_blacklist.clear();
    g_pending_path.clear();
    g_success_path.clear();
    g_initialized = false;
}

}} // namespace matter_engine3::retopo_blacklist
