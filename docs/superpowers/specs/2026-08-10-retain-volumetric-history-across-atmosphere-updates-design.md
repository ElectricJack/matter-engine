# Retain Volumetric History Across Atmosphere Updates

## Problem

Successful physical-atmosphere LUT commits currently invalidate the volumetric
scatter history. A history-invalid scatter frame still updates only one 2x2
Bayer phase, so movement that repeatedly crosses the atmosphere observer-height
threshold exposes a sparse dot pattern.

## Decision

Atmosphere lighting and LUT commits will no longer invalidate volumetric
history. Existing temporal blending will converge cached fog and cloud lighting
toward the newly published atmosphere over subsequent frames.

Invalidation remains mandatory when the froxel resource bundle changes or when
authored cloud shape/scatter settings change. Those cases alter storage or the
meaning of cached samples and cannot safely retain history.

Diffuse-GI and reflection-history decisions are unchanged.

## Implementation

`matter::atmosphere_history_decision` will stop setting
`reset_volumetric` for full atmosphere commits and atmosphere lighting changes.
The renderer continues to honor `reset_volumetric` if a future non-atmosphere
caller explicitly introduces such a decision.

## Verification

- A focused CPU test must prove full commits, direct/irradiance changes, and
  emission/disc changes do not request volumetric invalidation while preserving
  the existing diffuse-GI and reflection decisions.
- Existing shader/source and renderer tests must remain green.
- The real Vulkan atmosphere smoke must complete with zero validation errors.

