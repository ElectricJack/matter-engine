#ifndef MSL_MESH_SMOOTH_HPP
#define MSL_MESH_SMOOTH_HPP

#include "mesh_indexed.hpp"

#include <string>

// Taubin lambda/mu smoothing (shrink-free Laplacian) over the vertex 1-ring
// with uniform (umbrella) weights. Operates on MeshIndexed; connectivity is
// never changed. Boundary vertices (edge incidence != 2) are held fixed.
struct SmoothOptions {
    int   iterations = 2;      // >= 1
    float lambda     = 0.5f;   // positive step
    float mu         = -0.53f; // negative step (|mu| slightly > lambda)
};

struct SmoothResult {
    MeshIndexed mesh;
    bool        ok = false;
    std::string err;           // set when !ok
};

SmoothResult smooth(const MeshIndexed& in, const SmoothOptions& opts);

#endif // MSL_MESH_SMOOTH_HPP
