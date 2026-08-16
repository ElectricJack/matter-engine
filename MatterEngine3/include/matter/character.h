#pragma once

#include <cstdint>

#include "flecs.h"
#include "matter/math_types.h"

// Walking-mode character controller — ECS foundation (Milestone M0).
//
// This header declares the two public components and the fixed-pipeline system
// registration for a capsule character. M0 is deliberately the plumbing slice:
// the component model, the PrePhysics system seam, and the jump edge-latch
// semantics, all verifiable headlessly. Grounding, collide-and-slide, the 45°
// standability gate, and step traversal land in M2; terrain colliders in M1; the
// editor input bridge / camera in M3. See
// docs/superpowers/specs/2026-08-15-character-controller-design.md.

namespace matter::character {

// Tunables + engine-owned runtime state for one capsule character. The authored
// block describes the capsule and locomotion limits; the runtime block is what
// the fixed-step CharacterControllerSystem integrates and is captured in
// Play/Stop snapshots (M4).
struct CharacterController {
    // --- authored tunables ---
    float radius = 0.4f;                // capsule radius (m)
    float height = 1.8f;                // capsule total height (m)
    float move_speed = 4.5f;            // planar speed (m/s)
    float max_slope_cos = 0.70710678f;  // cos(45°): standable iff dot(n, up) >= this
    float step_up_height = 0.45f;       // tallest step the up-across-down trace climbs (m)
    float jump_speed = 5.0f;            // initial vertical speed of a grounded jump (m/s)

    // --- engine-owned runtime state ---
    Float3 velocity{};      // world-space; M2 moves the planar part to PhysicsVelocity
    bool grounded = false;  // set by the pogo-ray grounding query (M2)
    // Count of grounded jumps consumed. Diagnostic, and the observable that
    // proves the jump edge-latch fires exactly once per press across ≥2 fixed
    // steps (and is never dropped on a 0-step frame).
    uint32_t jumps_consumed = 0;
};

// Per-character movement intent, refreshed by whatever drives the character —
// the editor keyboard bridge, an AI/pathfinder, a script, or the network.
// Decoupling the controller from the intent source is the modular seam of the
// design: the controller reads this and never knows who wrote it.
struct MoveIntent {
    Float3 move_dir{};    // planar desired direction, world-space; sampled every fixed step
    bool jump = false;    // edge event: latched by the writer, consumed-and-cleared by the fixed step
    bool sprint = false;  // level-triggered modifier
};

// Registers the component reflection and the fixed-pipeline CharacterController
// system (PrePhysics phase, FixedPipelineSystem tag) into `world`. Safe to call
// once per world. Exposed directly so tests — and, until the M3 editor wiring,
// callers — can install the systems without importing CharacterModule.
void register_character_systems(flecs::world& world);

// Module wrapper mirroring PhysicsModule/StreamingModule, for when Runtime adopts
// the controller into its import list (M3). Registers under its own module scope.
struct CharacterModule {
    explicit CharacterModule(flecs::world& world);
};

}  // namespace matter::character
