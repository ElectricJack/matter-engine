#pragma once

// Test-only introspection for the retopo hook in part_flatten. Task 14's
// integration test asserts that a warm cache hit does NOT invoke
// MSL::retopo(): the .retopo.part sibling is read back in place of running
// the retopo pipeline again. The counter is bumped inside part_flatten's
// hook whenever MSL::retopo is actually called (not on cache hit / cache
// load). Production code doesn't reset it; it's cheap to leave enabled.
//
// Not thread-safe. flatten_part is single-threaded per-invocation and the
// test harness serializes bakes, so a plain integer counter is sufficient.
// If future work parallelizes flatten_part, swap the counter for an atomic
// (or gate the hook behind a per-thread stat block).

#include <cstdint>

namespace matter_engine3 {
namespace retopo_hook_stats {

// Reset the invocation counter to zero. Tests call this between phases so
// each phase's assertions see a clean baseline.
void reset();

// Number of times MSL::retopo was actually invoked (cache MISS path) since
// the last reset(). Cache hits (.retopo.part sibling present and loadable)
// do NOT bump this counter.
uint64_t invocation_count();

} // namespace retopo_hook_stats
} // namespace matter_engine3
