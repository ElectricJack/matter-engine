// MatterEngine3/src/world_lights.cpp
//
// Intentionally empty. `world_lights` is now a pure data namespace: everything
// it declares (world_lights.h) is a POD struct with in-class defaults and no
// out-of-line definitions, so this translation unit compiles to nothing.
//
// It used to hold the parser for manifest `light` lines. Manifest authoring was
// retired (commit 04b251ea) and lights moved into the world definition, where
// src/script/world_definition_loader.cpp reads `lights.sun` / `lights.sky` /
// `lights.spots` off the authored JS object; the provider converts those into
// world_lights::SpotLight (degrees -> cosines), and src/resolve_cache.cpp
// serializes the resulting WorldLights field-for-field into the resolve cache.
// The last unreferenced leftover here was removed by the dead-code sweep
// e7c19aae.
//
// The file stays in MatterEngine3/Makefile's source list so the namespace keeps
// a home if a runtime helper is ever needed again. It includes only its own
// header, which is the self-sufficiency check that still earns its keep; the
// vestigial part_asset.h / <vector> includes are gone.
#include "world_lights.h"

namespace world_lights {

} // namespace world_lights
