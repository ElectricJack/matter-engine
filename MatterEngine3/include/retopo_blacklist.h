// retopo_blacklist.h — persistent record of hashes whose retopo attempts
// crashed the process (typically autoremesher_core's LSCM geo_assert abort()).
//
// Mechanism: two-file journal in the cache dir.
//   parts/.retopo_pending — appended before each modifier_apply retopo call
//   parts/.retopo_success — appended after each modifier_apply retopo return
// A hash present in pending but not in success crashed mid-retopo; on the
// next process startup init() promotes it to the blacklist and future
// modifier_apply::apply_stack calls skip retopo for that region chunk.
//
// This is the "poor man's subprocess isolation" the Phase 5 design listed as
// an out-of-scope follow-up: process death IS the isolation boundary, and
// the on-disk journal captures which inputs to avoid on rerun.
//
// Thread-safety: g_blacklist is written only by init() and reset_for_tests(),
// so once init() has returned it is immutable and is_blacklisted() is a plain
// read. That is what lets modifier_apply.cpp test it BEFORE taking the static
// mutex it serialises retopo under. begin_attempt/end_attempt are NOT
// synchronised here; they are safe only because they are called from inside
// that mutex. Nothing in this module makes concurrent init() safe.
#ifndef MATTER_ENGINE3_RETOPO_BLACKLIST_H
#define MATTER_ENGINE3_RETOPO_BLACKLIST_H

#include <cstdint>
#include <string>

namespace matter_engine3 { namespace retopo_blacklist {

// Load journal files from <cache_root>/parts/, compute the blacklist as
// (pending - success), and hold it in memory. Idempotent; safe to call
// multiple times per process (the set is rebuilt from disk each time). Only
// the journal PATHS are retained: no file handle is kept open, and each
// begin/end_attempt reopens, appends, flushes and closes.
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


// Reset in-memory state (for tests). Does NOT touch the on-disk files.
void reset_for_tests();

}} // namespace matter_engine3::retopo_blacklist

#endif // MATTER_ENGINE3_RETOPO_BLACKLIST_H
