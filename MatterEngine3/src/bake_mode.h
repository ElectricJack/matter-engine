#pragma once
#include <cstdint>
#include <cstdlib>

// =============================================================================
// bake_mode.h — RUNTIME BAKE MODES, and the salt that keeps their bytes apart
// =============================================================================
//
// A mode here is an A/B switch over how the engine BAKES, selected at run time
// from the environment. That is a different thing from a version
// (version_vector.h) and the distinction matters:
//
//   a VERSION says "the rule changed, everything on disk is stale" and is
//   compiled in, so bumping it is a decision taken once and shared by everyone;
//
//   a MODE says "this process wants the other rule", and both rules stay valid.
//   Two processes with different modes must not read each other's artifacts,
//   but neither invalidates the other's.
//
// A mode that changed baked bytes WITHOUT reaching the cache key would be the
// exact bug version_vector.h was built to close, one level down: flip the flag
// against a warm cache and the engine serves the other mode's geometry, the
// A/B measures nothing, and the flag looks broken in a way that reads as the
// feature being broken. So every mode contributes to `salt()`, and `salt()` is
// folded into the part resolved hash beside the version vector.
//
// THE DEFAULT MODE MUST SALT TO ZERO, and the fold site must skip a zero salt.
// That is what keeps every existing key on disk bit-identical: turning a mode
// on is a cache miss for that process only, and turning it back off returns to
// the artifacts that were already there.
//
// Header-only on purpose. The salt is folded in part_asset_v2.cpp, which is
// linked by test binaries that have no reason to know what a terrain mesher is;
// a function-local static in an inline function gives one instance across every
// translation unit without adding a source file to six object lists.

namespace bake_mode {

// THE CONTOUR SEAM MESHER (docs/contour-seam-design-2026-08-13.md).
//
// On, a Y-tiled tile terminates its mesh exactly on its own six face planes,
// against a canonical contour that every tile touching the plane computes
// bitwise identically -- and the runtime seam welder has nothing left to do.
// Off, the tile bridges its -x/-z/-y borders by half a voxel and exports the
// boundary records and overlap bands the welder joins at run time.
//
// Default OFF while the mesher's tile-edge handover is unproven: the prototype
// gate (contour_mesh_tests.cpp) excludes cube-EDGE lines explicitly, so the one
// structural claim still unmeasured is what happens where three of a tile's
// faces meet. `terrain_mesher_tests` "contour: a 2x2x2 block" is that
// measurement; the default flips when it is green on the real mesher rather
// than on the prototype's two-tile fixture.
// TESTS ONLY. -1 means "ask the environment"; 0/1 force the mode.
//
// This exists so ONE process can mesh the same fixture both ways, which is the
// only way to separate a defect this rule introduces from one the mesher always
// had. contour_engine_tests found interior non-manifold edges on its first run
// and the question "were those there before?" is not answerable by two
// processes reporting two numbers about two different meshes -- it needs the
// same block, the same field, both rules, one comparison.
//
// Production must never call it. The mode is process-wide by design: two rules
// live in one cache only if something keys them apart, and `salt()` is that
// something -- it is read when a key is formed, so a mode that changed mid-run
// would name artifacts under one rule and fill them under the other.
inline int& forced_contour_seams() {
    static int v = -1;
    return v;
}

inline bool contour_seams() {
    if (forced_contour_seams() >= 0) return forced_contour_seams() != 0;
    static const bool on = [] {
        const char* e = std::getenv("MATTER_CONTOUR_SEAMS");
        return e && *e && e[0] != '0';
    }();
    return on;
}

// Fold into a cache key beside the version vector. Zero means "every mode is at
// its default", and a zero salt must NOT be folded -- see the note above.
inline uint64_t salt() {
    uint64_t s = 0;
    // One odd 64-bit constant per mode, XORed, so modes compose without
    // aliasing and adding one cannot disturb the others' keys.
    if (contour_seams()) s ^= 0xC047'0552'EA3D'0001ull;
    return s;
}

}  // namespace bake_mode
