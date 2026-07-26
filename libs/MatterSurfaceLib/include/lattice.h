#pragma once

// Phase 4 (Step 4) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// this header used to include raylib.h for Vector3. It is C++-only (no C
// consumer), so it uses matter_math.h's mm::Vec3 instead.
#include "matter_math.h"
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
    virtual mm::Vec3 slot_position(SlotCoord c) const = 0;
    // Adjacency offsets defining a slot's immediate neighbors.
    virtual const std::vector<SlotCoord>& neighbor_offsets() const = 0;
};

// Regular cubic grid: slot c sits at c * spacing; 6-connected (face neighbors).
class GridLattice : public Lattice {
public:
    explicit GridLattice(float spacing);
    mm::Vec3 slot_position(SlotCoord c) const override;
    const std::vector<SlotCoord>& neighbor_offsets() const override;
    float spacing() const { return spacing_; }
private:
    float spacing_;
    std::vector<SlotCoord> neighbors_;
};
