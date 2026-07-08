// retopo_blacklist.h — persistent record of hashes whose retopo attempts
// crashed the process (typically autoremesher_core's LSCM geo_assert abort()).
//
// Mechanism: two-file journal in the cache dir.
//   parts/.retopo_pending — appended before each MSL::retopo call
//   parts/.retopo_success — appended after each MSL::retopo return
// A hash present in pending but not in success crashed mid-retopo; on the
// next process startup init() promotes it to the blacklist and future
// apply_retopo_hook calls skip retopo for it.
//
// This is the "poor man's subprocess isolation" the Phase 5 design listed as
// an out-of-scope follow-up: process death IS the isolation boundary, and
// the on-disk journal captures which inputs to avoid on rerun.
//
// Thread-safety: single-threaded assumption. Meadow bakes parts sequentially
// via HostBaker, and apply_retopo_hook is only called from that path.
#ifndef MATTER_ENGINE3_RETOPO_BLACKLIST_H
#define MATTER_ENGINE3_RETOPO_BLACKLIST_H

#include <cstdint>
#include <string>

namespace matter_engine3 { namespace retopo_blacklist {

// Load journal files from <cache_root>/parts/, compute the blacklist as
// (pending - success), and hold it in memory. Idempotent; safe to call
// multiple times per process. Also opens the two journal files for append
// so subsequent begin/end_attempt writes are cheap.
void init(const std::string& cache_root);

// True iff `hash` was in pending but not success at last init() — i.e.,
// a previous process crashed while retopo'ing it. Memory-only lookup.
bool is_blacklisted(uint64_t hash);

// Append `hash` to the pending journal file (with fflush + fsync). Call
// BEFORE dispatching to MSL::retopo so a mid-call abort() leaves the hash
// recorded.
void begin_attempt(uint64_t hash);

// Append `hash` to the success journal file. Call AFTER MSL::retopo returns,
// regardless of the ok/!ok outcome — the "success" is that we returned at
// all, not that retopo produced useful output. Clean ok=false failures (bad
// input, timeout) will not be blacklisted.
void end_attempt(uint64_t hash);

// Number of blacklisted hashes (for stats / test hooks).
uint64_t blacklist_size();

// Reset in-memory state (for tests). Does NOT touch the on-disk files.
void reset_for_tests();

}} // namespace matter_engine3::retopo_blacklist

#endif // MATTER_ENGINE3_RETOPO_BLACKLIST_H
