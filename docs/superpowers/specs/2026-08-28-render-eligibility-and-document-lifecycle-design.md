# Render Eligibility and Documentation Lifecycle

**Date:** 2026-08-28

**Status:** Approved design

**Scope:** authored ray-tracing eligibility, raster-only animated water,
water-shader quality goals, documentation retirement, and roadmap maintenance

## 1. Direction

MatterEngine will support a simple authored `rayTraced` boolean on both part
definitions and part instances. Water is raster-only by default. Other
animated models remain eligible for ray tracing unless an author explicitly
opts them out; changing their default is a later performance decision.

The documentation set will distinguish active requirements from completed
history and obsolete designs. The root roadmap will contain only current work
and a small, explicit list of deferred decisions.

## 2. Ray-tracing eligibility

The public contract is one boolean rather than a renderer-mode enum:

```js
class AnimatedWaterSurface extends Part {
  build() {
    this.rayTraced(false);
  }
}
```

An individual placement may override the part default through the normal
placement options, including scene `PartInstance` declarations and child-part
placements:

```js
PartInstance: { part: "AnimatedWaterSurface", rayTraced: false }
```

Resolution order is:

1. explicit instance override;
2. referenced part default; and
3. engine default `true`.

The authored API remains boolean even though the engine must represent an
unset instance override internally to preserve inheritance.

A raster-only instance is omitted from the ray-tracing instance set and TLAS,
but remains in the ordinary raster pipeline and receives raster lighting and
shadows. A part whose resolved instances are all raster-only does not require
a BLAS. If another instance of the same shared part opts into ray tracing, the
shared BLAS may exist for that eligible instance; the raster-only instance is
still excluded from TLAS.

This setting replaces water-specific render-eligibility workarounds with a
general engine facility. It does not change visibility, shadow-casting, or
collision settings.

## 3. Animated-water rendering policy

Animated water is pinned to raster-only rendering for the current product
direction. Its visible animation must not trigger water-frame vertex decode
for ray tracing, animated-water BLAS construction or caching, or animated
water TLAS entries. The accepted static gameplay field and collision products
remain unchanged.

Raster water must be brought up to the visual standard of the surrounding
ray-traced scene. The active water-shader work is:

- screen-space refraction using opaque scene color and depth;
- depth-dependent absorption and fog while keeping shallow bottoms visible;
- foam driven primarily by the baked turbulence/whitewater field, with
  surface-shape cues used only as support;
- animated surface detail aligned with the baked flow field;
- convincing reflection from the raster-visible scene and environment; and
- correct raster shadows without duplicated RT or proxy shadows.

Acceptance requires representative screenshots at shallow water, rapids,
waterfall/plunge pool, and section handoff, plus matched frame timings showing
that animated-water BLAS work is absent. The goal is comparable composition
and material quality, not pixel identity with the old ray-traced water path.

## 4. Other animated models

Animated models may use `rayTraced: false` when BLAS update cost is not worth
the visual return. They remain ray-traced by default for now. A later matched
quality/performance study may recommend a different default, but this is a
roadmap decision rather than part of the water change.

## 5. Documentation lifecycle

The repository uses three document states:

- **Active:** Current authority or unfinished work. These documents remain in
  their present active locations.
- **Completed:** Implemented and still useful as historical evidence. These
  move under `docs/completed/` and are not active requirements.
- **Deprecated:** Superseded decisions or abandoned approaches. These move
  under `docs/deprecated/` and are candidates for later removal.

`docs/deprecated/README.md` will state that deprecated documents must not be
used as implementation authority. `docs/deprecated/MANIFEST.md` will record
each original path, retirement reason, and successor when one exists.
Completed documents will have equivalent directory guidance but will not be
described as wrong or deletion-ready.

Moves preserve useful filename and category structure where practical. Active
links are updated to current successors; links retained solely for historical
context may point into the completed or deprecated trees. Documents that
exist only on old branches are not imported merely to archive them.

## 6. Roadmap policy

The root `ROADMAP.md` will be replaced by a concise prioritized roadmap:

1. **Now:** general ray-tracing eligibility, raster-only animated water, and
   raster water shader quality.
2. **Next:** reliable longer sequential rivers, integration of the completed
   character-controller work, and the first focused gameplay playtest with
   floating craft.
3. **Scale before expansion:** renderer and publication bottlenecks proven by
   the implementation-gap audit, including changed-only TLAS maintenance and
   remaining publish/LOD work.
4. **Deferred decisions:** valuable missed ideas that require explicit product
   confirmation, including whether animated models should default to raster
   only.

The roadmap will not repeat completed milestones, preserve abandoned solver
directions, or treat every historical TODO as current intent. The
implementation-gap audit remains evidence for deciding what to retain, while
the roadmap is the current product commitment.

## 7. Cleanup safety

The cleanup is a sequence of moves and link updates, not deletion. Before a
document moves, it must be classified from its full contents rather than its
filename. Partially implemented specifications remain active until their
remaining requirement is completed, deliberately dropped, or extracted into
a smaller active document. The cleanup ends with a link scan, documentation
index update, and review of the resulting active-document inventory.
