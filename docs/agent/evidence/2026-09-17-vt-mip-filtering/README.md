# Adjacent-mip filtering experiment — deferred

This experiment is **not installed in the engine**. All affected production
shader/residency/test files were restored to their accepted `mountain-material-v5`
source hashes before the terrain POM recipe pass continued. The preserved
`candidate-v4.patch` includes shader filtering, parent-page demand, CPU overlap
coverage and proposed native GPU checks; it has not passed GPU acceptance.

- The existing sampler floors the mip; the terrain recipe also faded a positive
  aggregate mask toward zero. Both can contribute to distance transitions.
- A candidate blended color/ORM, decoded normals and metre-decoded height,
  including connected chart reconstruction and independent module/receiver
  scales. Parent feedback included normalized NPOT overlap.
- CPU parent coverage checks passed (`mip-filter-v2-test-cpu.log` in the sibling
  shared-VT evidence directory).
- Four native build attempts hit glslc/SPIR-V optimizer ID overflow in the
  G-buffer shader. No candidate editor was published or used for screenshots.
- Diagnostic compilation with `glslc -O0`, then SDK `spirv-opt` with a temporary
  16,777,215 ID bound and final ID compaction did complete. Its G-buffer was
  about 1.8 MiB versus the prior 1.1 MiB. This diagnostic is not a production
  compiler change or proof of runtime performance.

Next sampler work should reduce the connected POM/lookup compiler expansion,
then run the preserved fractional-LOD, physical-height, snapshot, receiver,
NPOT and native raster/RT gates. Do not call this filtering implemented.
The current visual pass instead keeps the validated sampler and improves
resolvable POM profiles and mean-preserving recipe filtering.
