# CPU raster staging growth diagnosis

Added opt-in MATTER_GEOMETRY_PAGES_PROFILE diagnostics to vertex/index staging
array capacity growth. Editor build passed (/tmp/raster-growth-build.log).
Strict scene audit C:/tmp/matter-blas-mountain/raster-growth-v1 passed: valid=True,
readiness=[{'ready_seconds': 10.958981500007212, 'confirmed_seconds': 25.99264399999811, 'camera_turn_seconds': None}]. This audit still measures full admitted-scene readiness; the new
visible-first target needs a separate visible-set readiness predicate.

Largest growth events (channel, old/new/live bytes, elapsed ms):

- vertex, 724975416, 1087463080, 725024608, 223.404
- vertex, 483316944, 724975416, 483354960, 124.755
- vertex, 322211296, 483316944, 322250984, 62.797
- vertex, 143205040, 214807560, 143287496, 62.122
- vertex, 214807560, 322211296, 214817680, 41.420
- vertex, 95470056, 143205040, 95812112, 36.733

The largest vertex resize copies hundreds of MB on the render thread. This
explains an observed ~223ms registration stall directly; it is CPU vector growth,
not evidence of a GPU transfer taking that long. Renderer GPU buffers have
upfront reservation, but CPU staging vectors lack matching reservation. Next
work must reserve/reuse CPU staging or remove that duplicate staging, without
shifting unreported work outside timing or causing runtime backing allocations.

User clarified the target as all in-frustum geometry and its VT detail first.
Recorded acceptance criteria in the virtualized procedural geometry design.
GPU geometry_cut.glsl already culls before fine-page feedback. Earlier sector
priority and end-to-end visible readiness are not yet verified.
