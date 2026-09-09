// libs/MatterSurfaceLib/src/lattice.cpp
//
// GridLattice: the only concrete Lattice implementation today. A lattice maps
// integer slot coordinates to positions in cluster-local space and declares its
// neighbour topology; see include/lattice.h for the interface and for why it
// exists (hex / diamond lattices would be new implementations of it).
//
// This one is a regular cubic grid: slot c sits at c * spacing (NOT the
// cell-corner convention Cell/Cluster use -- a slot is a point, not a box), and
// the six face-adjacent offsets are the neighbourhood used for shell detection.
// `spacing` is in world units (metres) and is fixed at construction.
//
// Stateless after construction and cheap to copy-free share: `slot_position`
// and `neighbor_offsets` are const and safe to call from any thread.
#include "../include/lattice.h"

GridLattice::GridLattice(float spacing)
    : spacing_(spacing),
      neighbors_{ {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} } {}

mm::Vec3 GridLattice::slot_position(SlotCoord c) const {
    return mm::Vec3{ c.x * spacing_, c.y * spacing_, c.z * spacing_ };
}

const std::vector<SlotCoord>& GridLattice::neighbor_offsets() const {
    return neighbors_;
}
