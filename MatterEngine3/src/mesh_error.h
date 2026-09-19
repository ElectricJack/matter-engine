#pragma once
#include "matter_math.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Offline, two-sided distance between triangle surfaces. Adaptive subdivision
// encloses the unsampled triangle interiors; a vertex-only maximum would miss
// holes and root/branch collars. Uses the engine's existing triangle BVH.
namespace mesh_error {
using Triangle=std::array<mm::Vec3,3>;
struct Config {
    double tolerance=0.001; // requested gap between sampled lower and upper bounds
    uint64_t max_queries=1u<<24;
    uint32_t max_depth=24;
};
struct Bounds {
    double lower=0,upper=0;
    uint64_t queries=0;
    bool depth_limited=false; // upper remains conservative when subdivision stops
};
bool measure(const std::vector<Triangle>& reference,const std::vector<Triangle>& candidate,
             const Config&,Bounds&,std::string& error);
// Offline attribute projection. Uses the same spatial index as error
// verification, with a bound on actual triangle tests (not all-pairs size).
// Equal-distance ties select the lowest source triangle index deterministically.
bool nearest_triangles(const std::vector<Triangle>& source,
                       const std::vector<std::array<double,3>>& points,
                       uint64_t& triangle_tests, uint64_t max_tests,
                       std::vector<uint32_t>& indices, std::string& error);
} // namespace mesh_error
