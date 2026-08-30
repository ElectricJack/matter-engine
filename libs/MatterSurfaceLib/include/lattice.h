#pragma once

// ---------------------------------------------------------------------------
// libs/MatterSurfaceLib/include/lattice.h
// ---------------------------------------------------------------------------
// The lattice abstraction used when a volume is filled with particles on a
// regular arrangement: it maps an integer slot coordinate to a local-space
// position and reports which slots count as a slot's immediate neighbours.
//
// Why the neighbour topology is part of the interface: interior culling
// (particle_culling.h) and occupancy queries (occupancy.h) decide whether a
// slot is on the shell by asking whether all of its neighbours are filled.
// That test only makes sense if the lattice itself defines adjacency, so the
// two travel together.
//
// Positions are in the same local space the Cluster's particles live in;
// `spacing` is therefore a local-space distance, and it is the "tier-0
// spacing S" that Cluster::set_base_detail_size() and StaticParticle's
// detail_size are expressed against.
//
// Usage: construct a concrete lattice (GridLattice today) on the stack and
// pass it by const reference to cull_interior() / emit_all(). Implementations
// are stateless apart from their parameters and hold no GPU or OS resources.
// neighbor_offsets() returns a reference to storage owned by the lattice, so
// it stays valid only as long as the lattice does.
// ---------------------------------------------------------------------------

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
    // `spacing` is the edge length between adjacent slots, in local-space
    // units -- the lattice tier-0 spacing S. Slot {0,0,0} sits at the origin.
    explicit GridLattice(float spacing);
    mm::Vec3 slot_position(SlotCoord c) const override;
    const std::vector<SlotCoord>& neighbor_offsets() const override;
    float spacing() const { return spacing_; }
private:
    float spacing_;
    std::vector<SlotCoord> neighbors_;
};
