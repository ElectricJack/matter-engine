# Solid SDF source meshing (opt-in version 1)

`matter/solid_sdf_meshing.h` is the host-facing field/job contract.
`render/gpu_meshing/gpu_solid_mesher_vk.h` supplies a persistent Vulkan service.
This path does not replace particle-water meshing or automatically translate
legacy `BuildBuffer` operations. Legacy smooth stage sets use different math.

The tape evaluates sequential pairwise polynomial smooth union/minimum,
difference/maximum with the operand negated, and intersection/maximum. A zero
blend width means exact min/max. The first operand must be a union. Negative
field values are solid. Boxes, externally rounded boxes, spheres, ellipsoids,
and Y-axis capsules are supported. World-to-local transform rows include
translation and must be proper rotations (reflection is rejected). Author physical
shape dimensions; affine shear, arbitrary displacement/noise, nested stage sets,
and multiple/blended materials have no representation in version 1. Do not
silently lower those authoring features into this tape.

The ellipsoid uses `(length(local/radii)-1)*min(radii)`: its zero set is the
ellipsoid, but it is a conservative radial field, not an exact Euclidean
signed distance away from the surface. CPU and GLSL evaluate that same field,
including through blends. Normals use central differences of the complete tape
at 2% of the requested voxel spacing. Near-zero gradients use an outward
geometric fallback. Classic marching cubes has unresolved ambiguous-cell cases;
central gradients at nondifferentiable hard creases do not yet provide an
explicit feature-splitting guarantee. Source quality acceptance must account for
these limitations; field parity alone is insufficient.

Bounds enclose all additive transformed primitive boxes, expanded by the total
smooth-union width times the largest ellipsoid aspect ratio plus two voxels.
Smooth difference/intersection cannot expand the current solid. This bound is
conservative, intentionally foregoing operand pruning. The rectangular lattice
uses the requested spacing in every axis and rounds each axis count upward.
Transforms whose float precision cannot represent that spacing are rejected.
Limits are explicit: 256 operands, at most 4,194,304 lattice samples and
2,097,152 output vertices, plus a checked 32-bit worst-case cell scan. A caller
may tighten both capacities. Exceeding output capacity reports `Overflow` and
leaves its previous result untouched; no truncated mesh is published.

Keep one `GpuSolidMesher` alive on the Vulkan owner thread, outside an active
frame. Calls are synchronous and must be serialized. Pipelines, scratch,
staging and query pools persist and grow with capacity. One immediate submission
records field sampling, cell classification/local scan, block scan, emission,
and final host copy. Cell and triangle offsets are deterministic; version 1
exports triangle soup with identity 32-bit indices. It is not zero-copy: the
final result owns CPU vectors and is ready for host serialization/adoption.

Version 1 copies the configured bounded output capacity, even when fewer
vertices are emitted, to avoid a counter fence before an exact-size copy.
Callers should choose sensible capacity budgets. The benchmark must compare
this overhead with a possible two-submit exact-size path before assuming one
submission is always faster. `SolidStats::gpu_ms` measures shader work, excluding
host transfer and decode; `submit_wait_ms` includes command preparation, queue
wait, GPU work and transfer. `host_ms` includes validation, resource preparation,
submission, decoding and mesh digest. GPU time is NaN if timestamps are not
available. No performance target has been demonstrated by source inspection.

Cancellation and generation checks occur before work, before submission, after
completion and before publication. They do not preempt an in-flight dispatch.
Failed submissions poison the service to prevent scratch reuse when completion
is uncertain. All referenced resources, including timestamp pools, participate
in device lifetime management and submission retention.

`run_gpu_solid_mesher_tests` covers all primitive evaluators, rigid transforms,
ordered operations, shallow ellipsoid relief/chips on a rounded brick, 6 mm and
3 mm grids, CPU/GPU field probes, surface residuals and normals, deterministic
repeat output, overflow and pre/post-submit cancellation/generation rejection.
Its changing-seed warm runs generate new meshes with no mesh cache. Report cold
initialization separately. A versioned tape contract is not a promise of
floating-point byte identity across drivers/devices: retain device/toolchain/
shader provenance alongside artifacts. Procedural noise, batching across jobs, edge sharing, explicit crease handling,
direct GPU mesh adoption and source texture projection remain later work.
The host integration below has its own native acceptance gate.

## Native service evidence (RTX 4090, MSVC RelWithDebInfo)

The initial single-submit path passed all conformance gates but missed host
latency badly: uncached coherent staging was decoded through scalar reads.
At 6 mm the warm cache-miss p50/p95 was 43.83/44.95 ms; at 3 mm it was
192.31/211.39 ms. Changing the preferred staging type to HOST_CACHED reduced
those values to 2.44/2.97 ms and 9.37/9.62 ms respectively, including output
vectors and content hashing. GPU shader p50/p95 was 0.217/0.218 ms at 6 mm and
0.542/0.569 ms at 3 mm. These are 12 changing-seed jobs per spacing; no mesh
cache is involved. Different host cadence can affect GPU clocks, so this is
not evidence of a shader arithmetic optimization.

The 3 mm fixture produced about 167,700 vertices from 380,205 samples and kept
36,457,472 bytes of Vulkan buffers resident. CPU/GPU field error was at most
2.98e-8 metres and normal dot agreement at least 0.99999976. All capacity,
determinism, cancellation and generation gates passed, with zero validation
errors. A forced coherent-memory path bulk-copies into persistent ordinary
RAM before decoding; it produced bit-identical positions, normals, indices,
material and digest. Its actual memory flags were 6; the preferred cached
allocation reported 14. The fallback copy took 7.53 ms in that bounded check.

Evidence lives in `C:/tmp/solid-sdf-a3-gate/cached/`, with before/after
source, shader and binary hashes, and the initial baseline is preserved in
`baseline-coherent/`. This service benchmark does not include ScriptHost,
BLAS construction, serialization or publication. The initial field-probe
setup warmed the pipelines, so it does not establish cold initialization time.

## Host authoring integration

`Part.solidSource(spec)` records one explicitly versioned source. `spec`
contains `version:1`, physical `voxelM`, optional bounded `maxVertices`
(default 500000), and up to 256 operations. Each operation supplies `shape`,
optional `combine` (default union), optional physical `centerM`, optional unit
quaternion `rotation` in xyzw order, optional `blendM`, and the physical shape
parameters named below:

* box: `halfExtentsM`
* roundedBox: `halfExtentsM`, `roundingM`
* sphere: `radiusM`
* ellipsoid: `radiiM`
* capsule: `radiusM`, `halfLengthM` along local Y

Unknown fields, non-unit quaternions, scale, reflection and incompatible legacy
geometry/modifier/animation combinations fail explicitly. The outer Part
transform is captured as a proper rotation and translation. Current material
and tint are retained. Generated normals travel directly into TriEx; no UV
coordinates are invented for a source that has none.

`ScriptHost::set_solid_source_baker` supplies the callback, cancellation control
and generation. Provider integration marshals it to the persistent renderer
service. An unset callback returns `solid-source-unavailable`; it never
silently substitutes a coarse CPU mesh. The host validates returned capacities,
indices, material and finite unit normals, checks cancellation after CPU packing,
and rechecks before static publication. Service, packing, BLAS and complete
host-bake time are distinct costs.

The opt-in `CastleStoneSource` uses `castle_solid_source.js`: eight recipes,
23 finite physical operations, exact default dimensions 0.30 × 0.14 × 0.20 m,
bottom at y=0, 3 mm sampling, one full source representation and no impostor.
It does not replace production castle geometry. The separate
`solid_source_host_tests` native executable is the integration gate for
serialized geometry, normals, dimensions, material, singleton metadata and
failure behavior. Its acceptance is separate from the service results above.

### Native host gate evidence

The visible MSVC `solid_source_host_tests` gate passed with exit 0 and zero
validation errors, including device shutdown. It baked all eight actual
`CastleStoneSource` recipes at 6 mm and 3 mm through ScriptHost and the real
GPU callback, then reloaded every `.bundle`. The source bodies contained
14,784 and 57,588 triangles respectively, with one full-resolution VARS entry,
no impostor, no descendants, and no extra compositional LOD ladder. Loaded
normals agreed with the original field to a dot product of at least
0.999999762; the dimensions remained 0.30 × 0.14 × 0.20 m with base y=0.
The unavailable-service, scaled/reflected transform, unknown-field,
fractional-version, malformed-normal and late CPU cancellation cases all
failed without publication. The last cancellation test reached the seventh
control check, immediately before static save.

This exposed a remaining end-to-end limitation: generating a full `.bundle`
through the existing BLAS and serialization path is substantially slower than
the mesher service. On the D-drive test cache, warm seeds 1–7 had median full
bake times of 118.18 ms at 6 mm and 343.79 ms at 3 mm, with maxima of
570.92/772.40 ms. Service medians were 4.47/16.60 ms; the maximum 3 mm service
sample was 22.72 ms. A preceding complete run had 3 mm full bakes around
257–277 ms. The first cold service call took 14.69 ms, within a 128.85 ms full
bake. Logs separately identify approximately 5–8 ms packing and 52 ms BLAS
registration for the detailed source. These timings include the D-drive cache
and do not establish C-drive or cache-independent serialization costs.

The 5–20 ms full-artifact target is therefore **not met** by this host path.
Further work must address BLAS/serialization and source-only ownership rather
than report GPU dispatch time as complete part-bake time. No production castle
replacement or final rendered source-quality acceptance follows from this gate.

Evidence: `C:/tmp/solid-source-host-gate/`; native bundles:
`build/qa/solid-source-host/parts/`. The final native process was PID 77604.
Before/after hashes retained the same executable and solid field sources; root
renderer source changed during the run without being relinked, and that source
change is explicitly visible in the receipt. The first run's native gate also
completed successfully, but its progress-forwarding wrapper lost its process
exit receipt on a Windows file-sharing error; it remains separately preserved
as `first-complete-native/`. The corrected shared-read monitor produced the
clean exit receipt and mirrored progress into the user's visible console.
