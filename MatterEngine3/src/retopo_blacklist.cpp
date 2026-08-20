// MatterEngine3/src/retopo_blacklist.cpp
// Implementation of the retopo crash blacklist declared in
// MatterEngine3/include/retopo_blacklist.h.
//
// The mechanism in one line: retopo (autoremesher_core) can abort() the whole
// process on some inputs, so the fact that a call RETURNED is recorded on disk,
// and an input the process entered but never came back from is skipped from the
// next launch onward.  Process death is the isolation boundary; these two
// journals are the only memory that survives it.
//
// State lives in process-wide globals rather than an object — there is one
// blacklist per process.  It is initialised from local_provider.cpp's
// autoremesher-only startup path (init(abs_cache_root_)) and read by
// modifier_apply.cpp immediately before each Retopo modifier.
//
// Files, both under <cache_root>/parts/ next to the .part artifacts, plain
// text, one zero-padded 16-digit lowercase hex uint64 per line, append-only and
// never rewritten or trimmed:
//   .retopo_pending — appended by begin_attempt() before dispatching to retopo
//   .retopo_success — appended by end_attempt() after retopo returns at all
// The blacklist is (pending - success), computed ONCE in init().  Nothing
// recomputes it mid-session, so a hash that crashes during this run is only
// skipped from the next run.
//
// Threading: g_blacklist is written only by init() and reset_for_tests(); the
// per-attempt calls touch the files, not the set.  is_blacklisted() is
// therefore a read-only lookup once init() has returned, which is what makes it
// safe for modifier_apply.cpp to call it outside the retopo mutex it takes a
// few lines later.  The append helpers are not themselves synchronised —
// modifier_apply.cpp serialises retopo under that mutex, so they are never
// entered concurrently.
//
// Growth: neither journal is ever pruned, so both grow by one 17-byte line per
// retopo attempt for the life of the cache directory.  Deleting them clears the
// blacklist.
//
// Failure policy: every filesystem error is swallowed.  A journal that cannot
// be opened for reading yields an empty set (nothing blacklisted); one that
// cannot be opened for appending silently records nothing.  Both degrade to "no
// crash protection", never to a wrong skip.
#include "retopo_blacklist.h"

#include <cinttypes>   // PRIx64 / SCNx64
#include <cstdio>
#include <string>
#ifndef _WIN32
#include <unistd.h>    // fsync (POSIX only; Windows viewer skips crash-safe fsync)
#endif
#include <unordered_set>

namespace matter_engine3 { namespace retopo_blacklist {

namespace {

// Process-wide state.  g_blacklist is the (pending - success) set computed by
// init(); the two path strings are the journals it was computed from.  All four
// globals are written only by init() and reset_for_tests().
//
// g_initialized gates every entry point: before init(), or after
// reset_for_tests(), is_blacklisted() returns false and the append helpers do
// nothing — an uninitialised blacklist never blocks a retopo and never records
// one.
std::unordered_set<uint64_t> g_blacklist;
std::string g_pending_path;
std::string g_success_path;
bool g_initialized = false;

// Load a journal file (one hex-uint64 per line). Silently returns empty on
// missing file. Malformed lines are skipped.
// Duplicates collapse because the result is a set, which is what lets the
// journals be append-only with repeated hashes.  The 64-byte line buffer is
// sized for the 17-byte lines append_journal writes; a longer line would be
// split across fgets() calls and its fragments parsed independently.
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
// Opens and closes the file on every call — nothing is held open between
// attempts, so the cost is one open/write/flush/close per retopo attempt and an
// unwritable path is silently a no-op.
//
// The fsync is POSIX-only.  On Windows the line is fflush()ed into the OS, so
// an abort() inside retopo still leaves the pending record on disk (that is the
// case this exists for) but a machine-level crash can lose it, in which case
// the offending hash simply is not blacklisted next run.
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

// Read both journals and recompute the in-memory blacklist.  Idempotent: the
// set is cleared and rebuilt from disk each time, so a second call picks up
// whatever has been appended since the first.
//
// It does NOT keep the journal files open afterwards — every append reopens
// them (see append_journal).  Reads both files in full, i.e. it is linear in
// the number of retopo attempts ever recorded under this cache root.
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

// Drops the in-memory state only; the journal files stay on disk.  Afterwards
// the module behaves as if init() had never run, so a test that wants a truly
// clean blacklist must also delete the journals or point cache_root somewhere
// fresh.
void reset_for_tests() {
    g_blacklist.clear();
    g_pending_path.clear();
    g_success_path.clear();
    g_initialized = false;
}

}} // namespace matter_engine3::retopo_blacklist
