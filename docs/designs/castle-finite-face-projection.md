# Finite source-face projection, version 1

The service projects the existing version-1 `SolidJob` field into a finite,
physical rectangle. It returns unlit floating-point samples for a caller to
place in an existing texture/atlas representation. It does not define another
texture file format, runtime UV system, Wang tile set, or lighting bake.

`matter/solid_face_projection.h` is the CPU/API contract;
`GpuSolidFaceProjector` is a persistent synchronous Vulkan implementation.
The source field comes directly from the A3 evaluator and `solid_field.glsl`.
No procedural recipe math is copied into this implementation.

## Sampling and normals

The proper orthonormal frame declares origin, U, V and N, with U cross V = N.
Rectangle and height coordinates are metres. Width and height are bounded
ceilings of rectangle extent divided by the requested metric pitch; actual
pitches divide the full extent by those dimensions and never exceed the request.
Samples use pixel centers. Coverage is binary at those centers; there is no
claim of area-filtered silhouette coverage or antialiasing.

A ray starts at `height_max_m` and travels toward `height_min_m` along -N.
Covered samples contain signed height along N and the outward field normal's
components in U/V/N. Front brick frames use +X/+Y/+Z; back frames flip U and N
together, preserving handedness. No instance scaling is involved. A back-face
normal facing the observer still has positive N component in its own frame.

The initial eight-source gate uses exact `castleStoneSourceSpec` jobs evaluated
through `ScriptHost::evaluate_solid_source` from `CastleStoneSource.js`, seeds 0–7,
physical dimensions .30 × .14 × .20 m, source spacing .003 m. Front and back
rectangles span .32 × .16 m around frame origin (0,.07,0), with signed height
interval [-.12,.12] m. Each patch can be one finite layer in the caller's atlas.
The gate first times all sixteen GPU projections, then runs dense CPU oracle
comparisons separately. Recipe capture, CPU oracle, serialization, compression,
texture upload, and rendering are excluded from service latency.

## Traversal and failure contract

The shared source validator gives conservative padded source bounds. Every ray
is clipped against that AABB and the caller's finite height interval. Bounds
come from the validated source rather than optimistic face extents. Empty
clipped intervals are misses. An interval beginning inside the source is an
explicit error because it cannot establish the outermost entry surface.

Version-1 primitives are 1-Lipschitz for exact proper rotations. The conservative
ellipsoid radial field multiplies normalized radius by its smallest radius.
Polynomial smooth CSG derivatives are convex combinations of its operands'
derivatives, preserving the Lipschitz bound. Validation admits small floating
orthogonality error; stepping uses 0.8 × positive field / 1.001 to retain margin.
Crossed zero brackets use explicitly bounded bisection. Otherwise a nonnegative
field residual within `hit_epsilon_m` is accepted. This is a field-residual
criterion, not a universal height-error bound near tangent or shallow-gradient
surfaces; analytic fixtures and CPU/GPU height comparisons gate actual quality.
Same-field central differences at `normal_epsilon_m` produce normals.

A ray is a miss only after its safe step exits the bounded interval. Exhausting
`max_steps` is a fatal `LimitExceeded`, never uncovered output. Nonfinite field,
undefined normal, failure to advance, clipped-inside entry, or malformed GPU
status fails the entire request. Caller output remains unchanged on any failure.
Explicit dimensions, pixel capacity, march/refinement limits, source limits,
frame precision, and right-handed orthonormal basis are checked before dispatch.
Worst-case pixels × (march steps + refinement steps + six normal samples) × source operations is additionally capped at
256 Mi operation-visits per request to reject impractically large dispatches.
Generation/cancellation is checked before preparation, submission, after readback,
and before publication; CPU work checks cancellation during each row. In-flight
GPU work is bounded and synchronous; cancellation suppresses publication rather
than preempting an already submitted dispatch.

## Resources, identity, and validation

The Vulkan service retains growable parameter, op, output and readback buffers,
a compute pipeline and timestamp query pool. Readback prefers HOST_CACHED memory;
when unavailable, one bulk copy into retained ordinary RAM precedes decoding.
Timing separates preparation, submit/wait, decode, GPU timestamps, and readback
copy. Device-lifetime controls retain buffers/pipeline/query objects for submits;
a failed submit poisons the service.

Recipe identity includes optional authored source identity (the ScriptHost resolved
hash includes selected imports, canonical parameters and engine versions), plus
the source field version/tape/physical spacing/material,
projection version, complete frame, rectangle, signed height interval, pitch,
tolerances and explicit budgets. Generation is a cancellation identity, not
content identity. No cross-job result cache is used for the sixteen fresh
projections.

Focused CPU tests cover analytic box/sphere height, front/back frame normals,
finite coverage, invalid scale/reflection/precision, bounded work, cancelled/stale
requests and preserved output on failure. The visible native GPU test captures
real recipes, compares every projected sample with the CPU oracle, checks fatal
budget exhaustion, cancellation/staleness and Vulkan validation errors. The native CPU projection and transient source-evaluation tests pass. GPU
conformance and measurements remain pending until the owner grants the shared
GPU lane.

## Transient production evaluation

`ScriptHost::evaluate_solid_source` now returns an owned existing
`DslState::SolidSourceRequest` payload with resolved source hash and field digest.
It shares the persistent baker's private executor: import folding, isolated
QuickJS context, canonical parameter merge, RNG seeding, build execution, and
final-state validation are common. The transient branch exits successfully
before BLAS/TLAS allocation, any GPU mesher callback or artifact publication.
No output directory is created. A source evaluation requires a positive time
budget (default 1000 ms); cancellation can interrupt executing JavaScript.

Only standalone source output is accepted. Child-asset requires, legacy/direct
geometry, modifiers, child placements, animation, terrain output and emitters are
rejected. An explicit empty requires array is allowed; accessor/function requires
is conservatively rejected. Shared-library imports are supported and participate
in the resolved source identity. Output ownership and generation survive the
QuickJS context; failure preserves the caller's prior result.

The GPU projection test now uses this successful production API, replacing its
earlier test-only callback/controlled-error capture. The native evaluation gate
passes real seeds 0–7, physical operations/material capture, import/seed identity
changes, zero GPU callback calls, no output files, cancellation/time budget and
invalid-output cases. Wiring transient dependency roles into the general part
graph remains a separate caller integration; ordinary bake_source still persists
its existing artifact normally.

For the current VT normal channel, reconstruct the source normal as
`U*n_u + V*n_v + N*n_n`, apply the rigid placement to wall-local coordinates, then
encode around the owning shell's local interpolated normal with the existing
`vt_normal_frame` convention. Copying the source patch's encoded RG directly
would mix different frames. Height and coverage remain explicit finite source
data; this service does not silently add height storage to the existing VT format.

## First wall integration through the existing detail atlas

A `defineMaterial(..., {detail:'CastleBrickBondDetail'})` request now recognizes
a declarative `Part.static params` descriptor with `detailBake:'brickBondV1'`.
The descriptor's `build()` is never called. Its merged parameters may import
shared JavaScript data normally. Typed parsing rejects unsupported keys, invalid
palette bytes, module traversal, incorrect variant count, and incompatible
period/pixel dimensions. `sourceModule` resolves through the provider's existing
ordered object roots; engine code contains no Castle module-name special case.

The current descriptor names eight source seeds and a physical running bond:
.30 × .14 m bricks, .3125 × .15625 m pitches, four columns/eight rows, and a
512-pixel square period. The period is physically 1.25 m. Sixteen genuinely
identical copies of that periodic tile form the existing 4×4 Wang transport;
finite source patches themselves are not relabeled as Wang-compatible tiles.
Integer GTex LOD metadata rounds actual 409.6 texels/m to 410; physical period and
sampling remain 1.25 m and 512 pixels. An explicit detailDensity must match the
configured integer density. The material API and runtime atlas loader stay in
place; there is no new texture format or UV pipeline.

On every lookup, `prepare_brick_bond_sources` evaluates eight source recipes in
memory and computes a key from the descriptor's folded identity, all eight
actual resolved source hashes and field digests, all sixteen finite projection
identities, and every versioned bond/palette field. This allows a valid final
GTEX cache hit to skip every GPU projection. On a miss, sixteen owned queue
envelopes project the faces, the CPU bond compositor builds the periodic image,
and only the final `.gtex` is saved. There is no source mesh, source BLAS,
intermediate source bundle, or box3d settle operation in this path.

`LocalProviderConfig::vk_solid_face_project` is the renderer callback. It is
marshaled through `gpu_run`; each pending projection owns its source tape, job,
frame, output and callback/control copies. Cancellation callbacks passed into
the deferred phase must themselves own everything they reference. The atlas is
not saved until all sixteen faces and composition have completed and the request
is current. Slot upload captures backend/arguments by value, propagates load
failure, and binds materials only after upload succeeds; failure unbinds affected
materials rather than reporting a successful textured publication.

The `brick-bond` log separates eight source evaluations, GPU projection host/GPU
sums and queue-inclusive wall time, CPU composition, final save/compression,
slot load/upload, and cache-hit status. These stages must be reported separately:
the existing 2048×2048 four-channel transport is about 40 MiB uncompressed, so its
compression/upload costs are not source-field generation latency.
