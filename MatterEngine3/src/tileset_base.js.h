#pragma once
// Embedded JS: Tileset root base class. Injected after part_base.js.h.
//
// A JS PRELUDE, compiled into the engine binary as a C string and evaluated
// into a QuickJS context before any user script runs. `script_host.cpp`
// evaluates `kPartBaseJS` (part_base.js.h) first and this second, so `Tileset`
// can extend `Part` and a tileset root inherits every part-level verb.
//
// MIND THE PRELUDE COUNT. This repo has several and they are NOT one namespace:
// `part_base.js.h`, `world_base.js.h`, the inline prelude built inside
// `world_definition_loader.cpp`, and this tileset-only one. This file is
// injected only on the tileset evaluation path (see the `kTilesetBaseJS` sites
// in script_host.cpp), so a global declared here is invisible to world scripts
// and to plain part scripts -- a helper that must exist everywhere has to be
// added to each prelude that needs it, or a world load dies with a bare
// ReferenceError.
//
// EVERY METHOD IS A THIN SHIM. `__dsl_ts_tile`, `__dsl_ts_base`,
// `__dsl_ts_layer`, `__dsl_ts_dropChild` and `__dsl_ts_variant` are native
// functions registered in `dsl_bindings.cpp`; they validate their arguments and
// record into the `TilesetState` / `TilesetSpec` attached to the current eval
// (tileset_spec.h). No logic belongs in this string -- put it in the binding,
// where it can fail closed with a real error message. The argument spreading
// here is load-bearing in one direction only: `tile()` unpacks the options
// object into five positional arguments, so adding a `tile` option means
// changing the binding's arity in dsl_bindings.cpp too.
//
// The literal is a raw string with the `JS` delimiter, so the JavaScript may
// contain quotes and backslashes freely but must never contain the sequence
// `)JS"`.
//
// CACHE NOTE. Editing this string changes how every tileset evaluates, but it
// is engine source and not script source: the settle and .gtex caches key on
// `matter_version::digest()` (version_vector.h) alongside the script source
// hash, so a behavioural change here needs a version-vector bump to invalidate
// what is already on disk.
static const char kTilesetBaseJS[] = R"JS(
globalThis.Tileset = class Tileset extends Part {
  tile(o = {})         { __dsl_ts_tile(o.size, o.texelsPerMeter, o.seed,
                                       o.edgeStripWidth, o.cornerClearRadius); }
  base(fn, mat)        { __dsl_ts_base(fn, mat); }
  layer(module, opts)  { __dsl_ts_layer(module, opts || {}); }
  dropChild(module, p) { __dsl_ts_dropChild(module, p); }
  variant(fn)          { __dsl_ts_variant(fn); }
};
)JS";
