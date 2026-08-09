#pragma once
// Installs the native __dsl_* DSL bindings (and the seeded Math.random override)
// onto a QuickJS-ng context. The context's opaque must point at a dsl::DslState.
#include <string>
struct JSContext;
namespace dsl {
void install_bindings(JSContext* ctx);

// Process-wide, monotonic census of the world-field DSL verbs. `build` (the
// authored build() call) is the largest phase of a sector bake, and it is two
// unrelated things: one native terrain-mesher call, and a scatter loop that
// calls heightAt/slopeAt/biomeAt once per candidate. Splitting them is what
// says whether to attack the mesher or the scatter. Readers take deltas.
struct TerrainVerbCensus {
    unsigned long long volume_calls = 0, volume_us = 0, volume_tris = 0;
    unsigned long long height_calls = 0, height_us = 0;  // heightAt/slopeAt/
    unsigned long long biome_calls  = 0, biome_us  = 0;  // moistureAt/biomeAt
};
TerrainVerbCensus terrain_verb_census();

// ---------------------------------------------------------------------------
// ScriptProfile — named aggregating timers that a bake script opens and closes
// itself, so the inside of build() stops being one opaque block.
//
// Why not BakeTrace. BakeTrace is a per-run span TREE, and the question that
// needs answering here spans thousands of independent runs: a streamed world
// bakes 6,547 sectors across a dozen worker threads, and no single sector's
// tree says where the aggregate went. What is wanted is the flat sum — this
// label cost N calls and T milliseconds across the whole fill — which is the
// same shape as TerrainVerbCensus above, generalized to labels a script names.
//
// The numbers are THREAD-milliseconds, summed across every worker, exactly as
// TerrainVerbCensus's `_us` fields are. A label reading 400,000 ms on a
// 12-worker fill that took 100 s wall is not a contradiction; do not divide by
// anything without knowing the worker count.
//
// Off unless MATTER_SCRIPT_PROFILE is set. When off, begin/end load one
// relaxed atomic and return, and the script side never looks a label up.
namespace script_profile {

// Ceiling on distinct labels. A bake script names its phases; sixty-four is
// far more than any ecology has, and the fixed table is what keeps begin/end
// down to an array index on the hot path.
constexpr int kMaxLabels = 64;

bool enabled();

// Intern `name`, returning its slot, or -1 when disabled or the table is full.
// Callers hoist this out of loops: it takes a lock and hashes a string, while
// begin/end take an int.
int slot(const char* name);

// Open/close a scope on THIS thread. Nesting is tracked so a label's `self`
// time excludes the labels opened inside it. `end` closes the innermost open
// scope; the slot argument is checked against it and a mismatch is counted
// rather than corrupting the stack.
void begin(int slot);
void end(int slot);

// Drop this thread's open-scope stack. A bake that throws out of JS leaves
// scopes open, so the bake entry point calls this to keep one failed bake from
// charging its remaining time to whatever runs next on the same worker.
void clear_thread();

// Zero every counter. Used to take deltas around a measured window.
void reset();

// Formatted table, sorted by self time descending. Empty when nothing was
// recorded, so callers can print it unconditionally and stay silent when the
// profile is off.
std::string report();

}  // namespace script_profile
}
