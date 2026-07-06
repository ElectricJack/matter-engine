#pragma once
// Embedded JS: Tileset root base class. Injected after part_base.js.h.
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
