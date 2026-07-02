# MatterEngine3 Docs

Concise architecture docs, written to support architectural decision-making.

- **[architecture.md](architecture.md)** — the bake pipeline: JS DSL → sessions →
  CSG/meshing → LODs → content-addressed `.part` assets; PartGraph composition;
  determinism model; known constraints.
- **[rendering.md](rendering.md)** — the viewer: per-frame world composition, the pure
  fragment-shader TLAS/BLAS ray tracer, GPU data layout, and a precise account of the
  current performance characteristics.
- **[authoring.md](authoring.md)** — writing part schemas, worlds, shared-lib modules,
  and what the test suite covers.

Core tension to keep in mind: the *bake side* fully delivers the original premise
(high-res procedural parts baked into deterministic, instanceable assets), while the
*render side* re-expands part hierarchies every frame and ray-traces everything in a
fragment shader — a reference-quality path, not a fast one.
