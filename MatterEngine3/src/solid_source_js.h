#pragma once
#include "dsl_state.h"
#include "quickjs.h"
namespace dsl {
// Shared strict decoder for build().solidSource and static finiteSurface.
// Does not alter DSL state. Semantic field validation belongs to the caller.
bool read_solid_source_recipe(JSContext*, JSValueConst, DslState::SolidSourceRequest&);
}
