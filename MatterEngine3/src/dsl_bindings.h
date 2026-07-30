#pragma once
// Installs the native __dsl_* DSL bindings (and the seeded Math.random override)
// onto a QuickJS-ng context. The context's opaque must point at a dsl::DslState.
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
}
