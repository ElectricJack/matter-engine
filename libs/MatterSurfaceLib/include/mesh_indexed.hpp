#ifndef MSL_MESH_INDEXED_HPP
#define MSL_MESH_INDEXED_HPP

// libs/MatterSurfaceLib/include/mesh_indexed.hpp
//
// `MeshIndexed` — the shared vertex/index mesh format used *between* stages of
// MatterSurfaceLib's mesh-transformation pipeline (simplify -> smooth ->
// retopo -> TriEx reprojection). It exists so those stages can hand meshes to
// each other without round-tripping through raylib's `Mesh` or through a
// non-indexed `Tri` soup.
//
// Where it sits: MatterSurfaceLib, above SpatialQueryLib — which despite its
// name owns the engine's core geometry types, including the `Tri`, `TriEx`
// and `float3` this header pulls from `tri.h`. `mesh_simplifier.hpp`,
// `mesh_smooth.hpp`, `mesh_retopo.hpp` and `mesh_transform.hpp` all speak
// this format; the non-indexed `Tri` form survives only at the BLAS boundary,
// which is exactly what `from_tri` / `to_tri` convert across.
//
// Conventions:
//   - Positions are in whatever space the caller supplies (cluster-local for
//     the cell mesher, part-local for a baked part). Nothing here transforms.
//   - `indices` is a flat triangle list, 3 entries per triangle — no strips,
//     no fans. `indices.size() % 3 == 0` is an invariant every consumer
//     assumes, and several validate.
//   - `triex` is optional and *per triangle*, not per vertex. When non-empty
//     it must satisfy `triex.size() == indices.size() / 3`; consumers treat a
//     size mismatch as "no TriEx" or as an error rather than repairing it.
//
// Gotchas:
//   - `from_tri` welds by quantizing each coordinate onto a grid of spacing
//     `epsilon` (see `src/mesh_indexed.cpp`), not by a true pairwise distance
//     test. Two vertices closer than `epsilon` almost always merge, but a
//     pair straddling a grid boundary can survive as duplicates. Accepted at
//     the 1e-4 default.
//   - Welding is position-only, so vertices sharing a position but differing
//     in shading normal are merged. Nothing is lost — normals ride on the
//     per-triangle `triex`, not on the vertex — but this is not an
//     attribute-split mesh.
//   - `from_tri` then `to_tri` preserves triangle order, but coordinates come
//     back from the welded representative (first-seen) vertex, so the round
//     trip is not bit-identical on positions.
//   - Plain data, no GPU or OS resources: freely copyable, and safe to build
//     and hand between threads like any other vector-of-POD.

#include "tri.h"  // Tri, TriEx, float3

#include <cstdint>
#include <vector>

// Shared indexed mesh format for MatterSurfaceLib's mesh-transformation
// pipeline. Both mesh_simplifier and mesh_retopo consume and produce this.
// Non-indexed Tri is used only at the BLAS boundary.
struct MeshIndexed {
    std::vector<float3>   positions;
    std::vector<uint32_t> indices;      // 3 per triangle
    std::vector<TriEx>    triex;        // optional; parallel to triangles
                                         // (size == indices.size()/3 when present)
                                         // empty vector = TriEx not attached
};

// Weld tolerance for from_tri. Default matches mesh_simplifier's existing
// internal weld (1e-4 world units).
struct WeldOptions {
    // World units. Coordinates are quantized onto a grid of this spacing, so
    // it behaves as a snap size rather than a pairwise distance test — see
    // `src/mesh_indexed.cpp`.
    float epsilon = 1e-4f;
};

// Weld the non-indexed Tri list into an indexed MeshIndexed. If `triex` is
// non-null it must be parallel to `tris` (size == tris.size()); triangles keep
// their per-triangle TriEx in the output.
MeshIndexed from_tri(const std::vector<Tri>& tris,
                     const std::vector<TriEx>* triex,
                     const WeldOptions& opts = {});

// Unweld back to non-indexed Tri, one triangle per 3 indices in emission order.
// `triex_out` is populated parallel to `tris_out` iff `in.triex` was populated.
void to_tri(const MeshIndexed& in,
            std::vector<Tri>& tris_out,
            std::vector<TriEx>& triex_out);

#endif // MSL_MESH_INDEXED_HPP
