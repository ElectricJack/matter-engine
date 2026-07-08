#pragma once
#include <cstddef>
#include <vector>

namespace tileset {

enum class ColliderType { Sphere, Capsule, Box, Hull };

struct ColliderFit {
    ColliderType type = ColliderType::Hull;
    float center[3] = { 0, 0, 0 };
    float axis[3][3] = { { 1,0,0 }, { 0,1,0 }, { 0,0,1 } };  // orthonormal, desc. extent
    float half_extent[3] = { 0, 0, 0 };                      // along axis[0..2]
    float radius = 0.0f;      // Sphere / Capsule
    float seg_half = 0.0f;    // Capsule core segment half-length along axis[0]
    std::vector<float> hull_points;  // Hull: xyz triples, <= 64 points
    float volume = 0.0f;      // analytic volume of the fitted primitive
};

// Fit a collision proxy to a vertex cloud (xyz triples) via PCA-OBB and an
// aspect-ratio heuristic. override_kind: nullptr/"auto" for the heuristic,
// or "sphere" | "capsule" | "box" | "hull" to force a type.
ColliderFit fit_collider(const float* xyz, size_t vertex_count,
                         const char* override_kind = nullptr);

} // namespace tileset
