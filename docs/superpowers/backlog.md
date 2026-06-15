# Backlog

Long-term ideas captured for later. Not scheduled. Each entry should eventually
graduate into its own brainstorm → spec → plan cycle.

## Draw-into-lattice authoring (multi-resolution lattices)

**Vision.** Build objects by *drawing primitives into a multi-resolution
lattice*, then mesh. Instead of hand-placing particles, an authoring algorithm
draws shapes additively and subtractively into occupancy:

- **Primitives:** sphere, capsule, block, line segment (extensible).
- **Additive / subtractive:** draws add or carve matter.
- **Per-primitive resolution:** some primitives (capsule, block) fill *every*
  resolution tier down to the configured finest detail; others (line segment)
  are drawn at a caller-specified resolution.
- **Mesh after authoring:** all draws complete, then the mesher runs.
- **Composition:** an object may be made from more than one lattice. Ideally
  parts are authored separately, baked to mesh(es), and reused.

**Integration point (already built).** The tiered surface lattice work
(`specs/2026-06-14-tiered-surface-lattice-design.md`) introduces a per-particle
`detail_size` / tier concept that drives mesh resolution. Today the tier is
*computed* from depth-below-surface. The drawing API will *author* it instead:
a primitive drawn at resolution `r` sets `detail_size = r` on the matter it
writes. Same field, different source — no rework of the mesh-resolution path
needed.

**Open questions for its future brainstorm:**
- Lattice representation: stay sparse `Occupancy`, or hierarchical/octree?
- How tiers are stored vs. computed once authoring writes them.
- SDF primitives in the mesher beyond spheres (capsule/box) — and whether the
  shared smooth-min field needs a harder `min` for faceted looks.
- Multi-lattice composition + part baking/reuse pipeline.

## Particle shapes beyond spheres

Rotated rounded-boxes / capsules as alternate particle primitives for richer
surface texture. Deferred from the 2026-06-14 appearance work; revisit only if
finer tiers + mesh resolution don't get the surface out of "looks like a wall."
Requires a box/capsule SDF in the mesher plus per-particle orientation threaded
through `StaticParticle` → cell carve → BLAS bake.
