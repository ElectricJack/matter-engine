#pragma once
#include "matter/solid_face_projection.h"
#include <string>

namespace gpu_meshing {
// Shared admission for cached geometry consumed by later material preparation.
bool validate_projected_face(const FaceJob &, const FacePatch &, Error &,
                             const BuildControl & = {});
// Intermediate finite geometry artifact, not a Wang atlas or final material.
// Versioned little-endian header carries frame, domain, pitch, height interval,
// source bounds and identity; lossless height/UVN normal/coverage follow it.
// The whole file has a checksum. Readers require the expected physical recipe.
bool load_projected_face(const std::string &, const FaceJob &, FacePatch &, Error &,
                         const BuildControl & = {});
bool save_projected_face(const std::string &, const FaceJob &, const FacePatch &, Error &,
                         const BuildControl & = {});
using SolidFaceProjector = std::function<bool(const FaceJob &, FacePatch &, FaceStats &,
                                             Error &, const BuildControl &)>;
struct FaceCacheStats {
    bool hit = false;
    double read_ms = 0, write_ms = 0;
    FaceStats projection{};
};
// Cache IO runs on the caller/worker; the callback may queue GPU preparation.
// Bad/missing cache gets one fresh preparation. Complete output is assigned
// only after success/currentness; an appearance recipe is deliberately absent
// from this geometry cache key. A warm hit needs no callback/backend.
bool load_or_project_face(const std::string &, const FaceJob &, const SolidFaceProjector &,
                          FacePatch &, FaceCacheStats &, Error &, const BuildControl & = {});
} // namespace gpu_meshing
