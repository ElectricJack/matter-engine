// MatterEngine3/src/modifier_apply.h
//
// Applies an ordered modifier stack (spec 2026-07-08 modifier regions) to one
// welded region mesh at part bake. Failure semantics: a modifier that fails is
// skipped with a one-line stderr warning; the rest of the stack still runs on
// the previous mesh (a stack of [{smooth},{retopo}] degrades to the smoothed
// mesh). Retopo is compiled only under MATTER_HAVE_AUTOREMESHER; otherwise it
// warns+skips so the Windows cross-build stays clean.
//
// How it fits: the bake pipeline welds a region's geometry, then hands the
// result here with the region's authored `dsl::ModifierSpec` stack. The three
// kinds map onto MeshChartingLib/MatterSurfaceLib primitives — simplify ->
// mesh_simplifier.hpp, smooth -> mesh_smooth.hpp, retopo -> mesh_retopo.hpp.
// Output feeds straight back into the bake, so anything non-deterministic here
// changes a part's content hash.
//
// Threading: callable from any bake thread. Retopo alone is serialized on an
// internal mutex (geogram's globals and the blacklist journal are single-caller
// state) and is pinned to one thread for determinism; the other kinds are pure.
#pragma once

#include "dsl_state.h"
#include "mesh_indexed.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace modifier_apply {

// Run `stack` in order on `mesh`. `chunk_label` prefixes warnings, e.g.
// "part 0123456789abcdef region 0 group 2". Never throws / never fails.
MeshIndexed apply_stack(MeshIndexed mesh,
                        const std::vector<dsl::ModifierSpec>& stack,
                        const std::string& chunk_label);

// Blacklist key for one region chunk's retopo attempt: FNV-1a fold of the
// welded mesh (positions flattened to tightly-packed floats — float3 may carry
// padding — plus index bytes) and the retopo params. Exposed for tests.
uint64_t chunk_retopo_hash(const MeshIndexed& mesh, const dsl::ModifierSpec& spec);

} // namespace modifier_apply
