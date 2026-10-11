#pragma once
#include <matter/terrain/field.h>
#include "terrain_field.h"

namespace matter::terrain::detail {
// Private borrowed view; the FrozenRegion must outlive the synchronous mesher.
struct MeshingField {
  const terrain_field::FieldRuntime &runtime;
  uint64_t sample_cost, retained_bytes;
};
MeshingField meshing_field(const FrozenRegion &);
}
