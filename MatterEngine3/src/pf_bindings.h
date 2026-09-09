#pragma once
// MatterEngine3/src/pf_bindings.h
//
// The particle-flow half of the bake DSL's JS surface. `install_pf_bindings`
// defines the raw `__pf_*` global functions on a QuickJS context; they wrap
// libs/ParticleFlowLib (`pf::Sim`, `pf::PathRecorder`) and are the only way a
// schema can run a particle simulation at bake time.
//
// Call it once per context, during binding setup — `dsl_bindings.cpp` does, at
// the end of its own install. It requires the context's opaque pointer to be
// the `DslState` for that bake, because every binding reaches through it for
// the sim registry, the error channel and the open voxel session.
//
// Schemas are not expected to call `__pf_*` directly: the JS prelude wraps them
// into the ergonomic `particleSim` / `pathRecorder` API. See pf_bindings.cpp
// for the handle, ownership and error conventions.
#include "quickjs.h"

namespace dsl {
void install_pf_bindings(JSContext* ctx);
}
