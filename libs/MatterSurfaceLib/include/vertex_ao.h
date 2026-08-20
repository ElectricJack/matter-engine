#pragma once


// libs/MatterSurfaceLib/include/vertex_ao.h
//
// Baked per-vertex ambient occlusion for cluster meshes. Instead of tracing
// rays, AO is estimated from the OCCUPANCY SLOT GRID that produced the
// particles in the first place -- nearby occupied slots darken a vertex. That
// makes it cheap, deterministic, and available at bake time with no GPU or
// scene context.
//
// How it fits:
//  - Run by `src/cluster.cpp` once a merge group's triangles exist, writing
//    into the parallel `TriEx` array (`ao0`/`ao1`/`ao2`, one per triangle
//    corner). `Tri` / `TriEx` are SpatialQueryLib types (`tri.h`).
//  - `pack_ao_w()` folds the three values into a single float's raw bits
//    (8 bits each) so they can ride in the `w` component of an existing row of
//    the BLAS triangle texture; the shader recovers them with
//    `floatBitsToUint`. Both sides must agree -- change one and you must
//    change the other.
//
// Conventions:
//  - AO values are normalized [0,1] with 1 = fully unoccluded; a triangle with
//    no `TriEx` entry is treated as unoccluded.
//  - `AoParams::radius` and `AoGrid::spacing`/`origin` are in cluster-local
//    units, the same space as the triangle vertices.
//  - `bake_vertex_ao` is pure CPU with no GL calls and no globals, and `tris`
//    / `triEx` must be parallel arrays of the same length and order.
//
// Design doc: docs/superpowers/plans/2026-06-20-baked-vertex-ao.md

#include "tri.h"        // Tri, TriEx, float3, make_float3
#include "occupancy.h"  // Occupancy, SlotCoord
#include <vector>

// Tunables for the AO bake.
struct AoParams {
    float radius   = 1.5f; // occlusion reach in cluster-local units
    float strength = 1.0f; // 0 = no darkening, 1 = full strength
};

// Maps a cluster-local position to the occupancy slot grid and back.
// slot_of(p) = round((p - origin) / spacing);  pos_of(c) = origin + c*spacing.
struct AoGrid {
    float spacing = 1.0f;
    float3 origin = make_float3(0.0f, 0.0f, 0.0f);
};

// For each triangle i, compute a per-vertex AO value in [0,1] from nearby
// occupied slots and write it into triEx[i].ao0/ao1/ao2. Pure; no GL calls.
// tris and triEx are parallel arrays (same length / same order).
void bake_vertex_ao(const std::vector<Tri>& tris,
                    std::vector<TriEx>& triEx,
                    const Occupancy& occ,
                    const AoGrid& grid,
                    const AoParams& params);

// Pack three [0,1] AO values into one float's raw bits (8 bits each), matching
// the shader's floatBitsToUint unpack. Exposed for the BLAS packer and tests.
float pack_ao_w(float ao0, float ao1, float ao2);
