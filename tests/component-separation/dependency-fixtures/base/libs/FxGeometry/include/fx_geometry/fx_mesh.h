#pragma once
// Fixture: geometry's public surface. Contracts only; no JS engine, no renderer.
#include <fx_contracts/fx_value.h>

namespace fx {
double fxgeo_triangle_area(const Value& a, const Value& b, const Value& c);
}  // namespace fx
