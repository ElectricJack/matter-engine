#pragma once

#include "raylib.h"
#include <vector>

// Integer coordinate of a lattice slot.
struct SlotCoord { int x, y, z; };

// A lattice maps integer slot coordinates to local-space positions and knows
// its neighbor topology (used for shell detection). Only GridLattice ships now;
// hex/diamond lattices become new implementations of this interface later.
class Lattice {
public:
    virtual ~Lattice() = default;
    // Base (un-jittered) local-space center of a slot.
    virtual Vector3 slot_position(SlotCoord c) const = 0;
    // Adjacency offsets defining a slot's immediate neighbors.
    virtual const std::vector<SlotCoord>& neighbor_offsets() const = 0;
};

// Regular cubic grid: slot c sits at c * spacing; 6-connected (face neighbors).
class GridLattice : public Lattice {
public:
    explicit GridLattice(float spacing);
    Vector3 slot_position(SlotCoord c) const override;
    const std::vector<SlotCoord>& neighbor_offsets() const override;
    float spacing() const { return spacing_; }
private:
    float spacing_;
    std::vector<SlotCoord> neighbors_;
};
