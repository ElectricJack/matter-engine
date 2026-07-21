// bake_trace_names.h — canonical span-name constants for BakeTrace
// (docs/bake-lab.md §II.1). Span names are the diff / variant-table key:
// call sites must use these constants, never string literals, so traces
// from different runs and builds match by name. Extend this list as new
// bake phases arrive; never repurpose an existing name.

#pragma once

namespace bake_trace {

// execute_bake stages (matter_engine.cpp).
inline constexpr const char* kSpanInstall = "install";
inline constexpr const char* kSpanCompose = "compose";
inline constexpr const char* kSpanPublish = "publish";

// compose_world internals.
inline constexpr const char* kSpanScatter     = "scatter";
inline constexpr const char* kSpanTileset     = "tileset";
inline constexpr const char* kSpanSettleLayer = "settle-layer";

// Per-part bake (script_host.cpp bake_source): parent + phase children.
inline constexpr const char* kSpanPartBake = "part-bake";
inline constexpr const char* kSpanFold     = "fold";
inline constexpr const char* kSpanCtx      = "ctx";
inline constexpr const char* kSpanEval     = "eval";
inline constexpr const char* kSpanBuild    = "build";
inline constexpr const char* kSpanMesh     = "mesh";
inline constexpr const char* kSpanSave     = "save";

// LOD ladder + flatten (lod_bake.cpp, part_flatten.cpp).
inline constexpr const char* kSpanLod     = "lod";
inline constexpr const char* kSpanLodRung = "lod-rung";
inline constexpr const char* kSpanFlatten = "flatten";

}  // namespace bake_trace
