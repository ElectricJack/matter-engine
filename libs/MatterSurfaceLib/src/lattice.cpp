#include "../include/lattice.h"

GridLattice::GridLattice(float spacing)
    : spacing_(spacing),
      neighbors_{ {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} } {}

Vector3 GridLattice::slot_position(SlotCoord c) const {
    return Vector3{ c.x * spacing_, c.y * spacing_, c.z * spacing_ };
}

const std::vector<SlotCoord>& GridLattice::neighbor_offsets() const {
    return neighbors_;
}
