#pragma once
// Fixture: graphics' public surface. Contracts only; no geometry.
#include <fx_contracts/fx_value.h>

namespace fx {
unsigned fxgfx_pack_color(double red, double green, double blue);
double fxgfx_luminance(const Value& color);
}  // namespace fx
