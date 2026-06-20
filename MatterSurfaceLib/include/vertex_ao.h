#pragma once

#include "bvh.h"        // Tri, TriEx, float3, make_float3
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
