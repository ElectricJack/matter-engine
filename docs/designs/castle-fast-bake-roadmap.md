# Fast procedural architecture: source parts, surface detail, and assemblies

Date: 2026-09-11. Owner: Astra. Status: implementation roadmap; performance targets below are targets, not measured results.

## Outcome

Build realistic, walkable castles without minutes of work per repeated building block. Begin with one brick, one timber, and one floor slab. Retain the detailed procedural source and derive reusable runtime representations. A wall is a structural module with apertures and material detail, rather than thousands of independently published brick meshes. The same system must handle angled and curved walls, stairs, floors, and timber assemblies.

The user explicitly prefers GPU meshing of high detail brick SDFs. Implement and benchmark that option. Do not assume that moving an existing expensive algorithm onto the GPU removes dependency resolution, allocation, synchronization, simplification, texture projection, serialization, or publication costs.

## Starting evidence

The castle/render integration is merged with upstream at `44220e96` and pushed to main. The native MSVC PhysX editor links. Ten affected castle suites pass after resolving overlapping connector, court, roof, and test changes. Renderer code is identical to the source-pinned native direct/GI and transmission gates that reported ALL PASS and zero validation errors. Final full-castle walkthrough and full-resolution GI stability acceptance from the earlier project remain incomplete.

Measured implementation checkpoint (2026-09-11; full editor integration is still pending):

* The frozen native 4 m timber baseline spent 37.5–38.9 s in meshing. Conservative CPU candidate bounds plus the normal fix reduce this to 5.17–5.68 s with the same 40,620 triangles. This is an intermediate fallback improvement, not the final source-generation target.
* A simple slab generates 12 triangles in about 0.03 ms, but four durable bundle writes spend roughly 93–103 ms in flushes on warm fresh bakes. Combining two sections reduces four publications to three; variable flush latency means that intermediate change alone has not demonstrated an elapsed-time improvement. A static singleton transaction is under test to combine all four sections into one publication.
* The normal flattened castle path already stores one LOD for `[1]`. The compositional staging fallback incorrectly builds three; the source-owned singleton policy must cover disk and in-memory staging identically.
* The new Vulkan solid-field service passes its field/normal/determinism/overflow/cancellation gates with zero validation errors. Warm fresh recipe generation at 6 mm has p50/p95 2.436/2.971 ms; at 3 mm, 9.373/9.615 ms. These include service preparation, GPU submission/wait, readback and CPU mesh conversion, **not** QuickJS, BLAS construction, bundle persistence or scene publication. Cached host readback fixes an initial 192 ms median at 3 mm; GPU dispatch itself is about 0.54 ms in the corrected run. Cold initialization is not included in these numbers.
* Native QuickJS `CastleWingMasonry.requires()` takes 3.8–4.5 s, with 20 dependencies. Rebuilding whole-site data inside a leaf lookup is a separate measured bottleneck. The immediate safe change is selected-wing decoration; the new structural kit should consume compact module recipes.
* New physical shell catalogues and rigid span planning are implemented in shared JS. Translation/proper rotation are enforced; beams repeat fixed lengths and keep one explicitly cut residual. Native leaf shells have passed their first geometry gate; the attributed wall/curve gate and rendered proof are still pending.

Raw native profiling and provenance are documented in [the native part report](../castle-native-part-profile-2026-09-11.md). GPU receipts are in `C:/tmp/solid-sdf-a3-gate/cached`, with the uncached baseline in its `baseline-coherent` sibling.

Initial source observations that motivated this roadmap:

* `CastleStone.js`, `CastleBeam.js`, and `CastlePlank.js` already declare `lodBudgets = [1]` and `noImpostor = true`. Confirm the actual persisted and published rung count, including legacy fallback paths, before attributing time to unwanted LODs.
* The production gallery uses eight ordinary brick recipes, but roughly 58,000 brick placements. Geometry recipes and placements are different cost axes. Diagnostic `CastleMaterials` still displays twelve seeds; it must not expand the production catalogue.
* `castle_primitives.js::emitStone` emits a box, twelve oriented bevel cutters, ellipsoid face relief, chips, and grooves, then runs a 0.34 simplification modifier. Its fine session requests 6 mm at detail 1. All sessions in that region ultimately share the smallest requested spacing.
* `script_host.cpp::mesh_sdf_ops` processes those fields in fixed one metre cells. Additive fat primitives allocate candidate cells from a sphere of twice their bounding radius. A slender beam can therefore generate a large candidate cube around a thin solid. Every candidate then evaluates a bounded cubic lattice. Measure created, rejected, sampled, and active cells separately.
* That path passes `max_pow=6`; `choose_absolute_division_pow` samples a one metre cell at no finer than 1/63 m, about 15.87 mm. Thus a requested 6 mm brick detail is not actually guaranteed by this CPU path. Simply increasing this cap would increase cubic work and risk the old 16-bit mesh limit.
* `surface.c::GenerateMeshInternal` evaluates ordered fat-primitive CSG, but its final `compute_surface_normals_impl` invocation has no ordered stages or fat primitive arguments. Sphere-only analytic normals cannot describe all these surfaces. Trace normals through extraction, modifiers, indexed assets, raster vertices, texture projection, and RT hit interpolation; fix the first broken contract rather than smoothing every hard edge indiscriminately.
* `docs/castle-prebake-js-profile-2026-09-11.md` found roughly one second of Node world construction, but fresh QuickJS contexts repeatedly rebuild site manifests during dependency resolution. Node timing is not native QuickJS timing. Cache work must respect deterministic inputs, source edits, reload generations, and per-world material handles.
* Earlier native corrected-castle evidence measured about 439 s install and 455 s publication, with 44 baked parts and 203 hits. These aggregate phases include many activities and do not prove meshing is the only problem. Old terrain measurements likewise found staging/LOD/reprojection more expensive than initial meshing; those are historical guidance, not current castle measurements.
* The existing Vulkan visual mesher supports particle water, not arbitrary ordered solid CSG. Its accepted small-water benchmark was roughly 36 ms warm, including multiple synchronous submissions and readbacks; cold pipeline initialization was roughly 200 ms. Reuse infrastructure, but do not transplant that scheduling model unquestioned into thousands of small parts.

## Architecture decisions

### 1. Separate four products

1. **Recipe:** compact deterministic authoring inputs, references, and transforms.
2. **Detail source:** high resolution geometry or evaluable field, built once per shape variant. It need not be drawn or have a runtime LOD chain.
3. **Runtime surface:** a bounded low complexity structural mesh plus material textures, with explicit near-detail geometry where needed.
4. **Collision:** simple independent structural volumes preserving floors, walls, clearances, and portals.

Source-only ownership must be explicit. A brick also placed loose in the world still needs a drawable representation. A brick used only by a wall's texture bake should not consume runtime instance slots or accumulate unused child LODs. Keep source availability for rebakes without recursively expanding it into the runtime scene. Bake-only inputs default to transient memory, with zero intermediate bundle writes. Reuse them by recipe identity within a bake generation, then release them after their consumers finish. Persist the final structural mesh and surface atlas. For an evaluable SDF, project the field directly into the reusable face atlas; do not build a triangle mesh or BLAS merely to project it back into a texture. A source also placed as a loose object has a separate drawable demand, so mixed use cannot erase its runtime output. Source edits must still invalidate dependent outputs even when no source artifact exists on disk.

Keep five to ten ordinary brick shapes. The current eight effective recipes satisfy this constraint. Longer-term separate reusable geometry identity from palette identity where material binding permits it; do not silently remove a material from an existing cache key. Seeds, geometry inputs, algorithm versions, and source dependencies remain part of identity.

**Placement uses translation and proper rotation only: no instance scaling.** Author each catalogue shape at its physical dimensions. Assemble longer spans from repeated fixed-size pieces, using a bounded catalogue of shorter pieces for gaps (for example 1/2/4 m timber), or an explicitly authored cut end when required. A rotated 4 m beam remains the same geometry resource. Do not stretch normalized cubes, bricks, beams, floor slabs, or whole assemblies to fit. Preserve section thickness, brick proportions, joint spacing, and physical texture density. The existing castle contains scaled placements; migrate those consumers explicitly and test the final emitted transforms rather than claiming the new catalogue alone removes scaling.

### 2. GPU SDF source generation

Add a bounded solid-field job alongside the existing particle-water mesher. A compact validated op tape describes transformed boxes, rounded boxes, spheres/ellipsoids, capsules/cylinders, ordered union/difference/intersection, blend widths, and deterministic procedural displacement. Begin with the subset needed by a chipped brick. Unsupported operations fail explicitly or use a labelled CPU fallback; never approximate them silently.

Use tight object-local rectangular bounds. Start with dense rectangular sampling for small bricks, then use coarse classification/active tiles for large or thin domains if measurements justify it. A conservative support bound must include smoothing and displacement; arbitrary composed fields cannot be culled from an unjustified distance estimate. Maintain a common lattice at tile boundaries.

Classify, prefix-scan, emit, and compute normals from the SAME field. Use 32-bit output indexing, explicit capacity checks, no truncation, and deterministic output offsets. Prefer sharing edge vertices to exporting triangle soup when the measured cost is worthwhile. Near-zero gradients need deterministic geometric fallback. Hard CSG creases must remain sharp; smooth blends must shade continuously. Material selection must follow the same winning/blended field semantics as geometry.

Keep pipelines and reusable scratch alive. Batch small independent jobs and amortize allocation and submission. Avoid a fence and CPU counter readback after every trivial dispatch. Record GPU timestamps separately from host wall time and queue wait. A final host artifact copy is acceptable initially; the API should allow later direct GPU adoption. Do not claim zero-copy while building a second CPU mesh for serialization.

Provide CPU conformance probes, no-GPU behavior, cancellation/generation rejection, overflow tests, and a versioned artifact identity. Cross-device provenance must not be confused with a guarantee of floating-point byte identity. Preserve the existing water path and its acceptance tests.

### 3. Normals before cosmetic detail

For ordered fields, compute analytic/automatic-differentiation gradients where straightforward, or central differences of the exact evaluator as the correctness baseline. Sample all CSG operations, transforms, and blends. Use an appropriate local epsilon; preserve sharp feature splits rather than averaging across unrelated faces or materials.

Audit simplification/reprojection. The brick currently incurs simplification inside its sole source representation. A source used only for texture projection may need no QEM at all. Direct low-poly primitives should receive explicit normals and UVs. Reusing the SDF source's normals on a low-poly envelope requires a normal texture, not copying one source triangle normal to an entire wall polygon.

Raster and RT must agree on the surface shading frame. Preserve geometric normals independently for ray offsets, visibility, and backface handling. Validate with a sphere, transformed ellipsoid, rounded/chipped box, carved recess, and a flat beam under a moving grazing light. Do not judge shading using only a bright frontal camera.

### 4. Assemblies built from known structure

Avoid generic QEM over tens of thousands of touching bricks when the floor plan already describes the desired envelope. Generate the runtime wall shell directly from the plan: front/back surfaces, closed ends, top/bottom caps, window/door reveals, arches, and exposed corner stones. Use modest subdivisions where silhouette, curvature, displacement, or shadows demand them.

Wall modules span ordinary one-metre grid intervals, with canonical 1/2/4/8 m runs where practical, storey heights and finite openings. Endpoint frames support 15/30/45 degree joins. Curved sections use an arc-length parameterization and a chord-error bound; join sockets declare position, tangent, thickness, elevation, and material course phase. Do not key each wall by its entire castle manifest or arbitrary world position.

Bake albedo, tangent/object-space normal with an explicit transform convention, roughness/metallic, and height from the detailed brick layout into the wall's local parameterization. Macro geometry remains in the shell; micro relief belongs in these channels. Bake material response, not sun/local-light illumination. Optional bounded AO/cavity is distinct from dynamic GI and must not double-darken it.

Reuse the engine's chart-VT and Vulkan geometry-to-texture facilities where their contracts fit. The existing periodic ground `.gtex` baker is not automatically an arbitrary wall baker: it projects from above and has a particular normal convention. An explicit projection frame, finite chart bounds, gutters, edge dilation, and mips are required for vertical walls and reveals. Preserve physical texel density under placement transforms.

Prefer reusable brick-face detail atlases and GPU stamping/composition for repeat wall patterns, avoiding a full high-poly ray bake for every wall. For unique damage and irregular edge geometry, project against a source-only instance hierarchy with shared brick BLAS. Compare stamping and projection on the same output resolution and error criteria. Unique large textures scale with output texels; unlimited-resolution atlases cannot have a constant millisecond cost.

Do not flatten gold fittings, glass panes, deep recesses, or large broken edges into an opaque wall texture. Keep separate meshes/materials where transmission, silhouettes, or important shadowing require them. At very close range, retain exposed brick edge geometry or a bounded near-detail representation; pure normal mapping cannot reproduce all parallax, silhouettes, contact shadows, or secondary-ray intersections.

Floor slabs use planar shells and stone/wood detail textures. Beams use inexpensive beveled prisms, explicit joinery and end geometry, with longitudinal grain and end-grain surface maps. SDFs are valuable for broken ends, mortises, splits, knots that alter silhouette, and carved ornament; meshing every wood fibre is usually more expensive than its visual benefit. Benchmark a GPU-authored source against direct meshes at matched visual quality.

### 5. Dependency and publication costs

Measure module load/evaluation, parameter canonicalization, `requires`, hash calculation, bake, artifact encode/write/read, LOD/BLAS/chart construction, mesh conversion, upload, and scene publication independently. Report unique recipes, dependency visits, child placements, output triangles, texels, and bytes beside time.

Prefer compact per-module descriptors over reconstructing whole decorated castles in every `requires()` and `build()` call. Reuse immutable resolved data within a well-scoped install transaction. Do not keep arbitrary live JS objects across contexts. An engine cache must be bounded and keyed by exact source/params/dependencies, with failures and invalidation covered. Rendering and collision should consume the same compiled plan snapshot where appropriate.

Cache intermediate products separately when their inputs differ. Preserve valid source meshes when only a wall layout changes, preserve wall geometry when only a palette changes, and avoid rebuilding source BLAS for every texture projection. Reuse staged memory for immediate publication instead of avoidable disk round trips. Require measurements before broad cache or format migrations.

## Measurement contract and targets

Run native MSVC on the RTX 4090. Use a frozen fixture suite: ordinary brick seeds, thin and long beams, floor slab, solid wall, window wall, angled join, curved wall, then one wing and a whole site. Record toolchain/source/shader hashes, dimensions, spacing, achieved sampling resolution, triangle counts, materials, texel density, and requested/actual LODs.

Separate cold process/pipeline initialization, warm-service **cache-miss** baking, disk-cache hit, and repeated instance placement. Report p50/p95 over repeated jobs, full batch throughput, CPU wall time and GPU execution; never label cache-hit lookup time as fresh generation time. Include matched quality screenshots and geometric/normal errors. Logging must not overwhelm the benchmark.

Initial engineering budgets, revised only with explicit evidence:

| Product | Warm cache-miss target | Other constraint |
| --- | --- | --- |
| Simple runtime prism/slab | 1–5 ms end to end | No voxel sampling or QEM |
| Detailed source brick | 5–20 ms end to end; 1–5 ms GPU work | Requested 3–6 mm detail actually represented |
| Canonical textured wall module | 5–30 ms after shared sources exist | Report texels and include projection/composition |
| Cache-hit part acquisition | Under 1 ms amortized | Include decode/upload separately |
| Repeated placement | No rebake | Shared mesh/texture/BLAS storage |

Cold initialization, optional offline texture compression, very large unique textures, and full-world setup receive separately reported budgets. If a target fails, identify the stage and keep working; do not hide it behind a different definition of "part". Runtime acceptance compares frame CPU/GPU time, geometry/texture/AS memory, instance counts, and visible detail in both raster and native RT at the same camera and resolution.

## Implementation order and ownership

Root owns integration, roadmap changes, shared build manifests, canonical native builds/GPU scheduling, and acceptance. All implementation agents are fresh Astra subagents with no conversation fork, as requested. Agents may delegate bounded independent subtasks when slots exist. Use disjoint source ownership, send concrete evidence, and do not mutate integration Git. Implementation is reviewed before adoption; failed work is revised rather than counted complete.

| Task | First deliverable | Gate/dependency |
| --- | --- | --- |
| A1: Native part profiler and dependency diagnosis | Repeatable brick/beam/slab benchmark, per-stage costs, actual rung census; measured safe dependency optimization if justified | Establish baseline before source changes |
| A2: Ordered-SDF normals and CPU candidate bounds | Correct gradient semantics, conservative thin-solid cell bounds, regression tests and matched timing | Preserve geometry except intended normal fix |
| A3: GPU solid-SDF mesher | Bounded chipped-brick op tape, CPU oracle, GPU extraction, timings, overflow/fallback tests | New path opt-in until parity/quality gates |
| Root A4: Assembly surface contract | Reusable wall/floor/beam shell and texture projection design mapped to live facilities | Avoid incompatible duplicate texture systems |
| B1: Source-part integration | DSL/host handoff to GPU jobs, one source rep, persistent service and batched jobs | A1/A3; root coordinates host ownership |
| B2: Structural module kit | Direct wall/portal/arc shells and floor/timber surfaces from shared JS | A4, existing collision/plan tests |
| B3: Surface-detail baking | Shared source atlases, finite chart projection/composition, normal/height/ORM sampling in raster and RT | A3/A4, small module proof first |
| C1: Large assembly integration | Source-only descendants, bounded module catalogue, efficient publication and cache reuse | B1–B3; no invisible geometry regression |
| C2: Native acceptance | Small-part comparisons, one wing, then castle; cold/warm/runtime reports and screenshots | Root reviews actual rendered result |

A1–A3 may proceed concurrently after this roadmap exists. Native GPU runs and canonical builds are serialized so timings and binary provenance remain trustworthy. Do not launch full castle bakes until small parts and one wall pass. Future batches are adjusted from measured results, not from an assumption that every proposed technique must ship.

## Alternatives considered

* **GPU everything:** valuable for dense source fields, but queue overhead dominates tiny jobs and textures may represent fine grain better. Use a hybrid.
* **Simplify all assembled brick triangles:** generic but wastes source construction, QEM, and correspondence work on topology already known from the plan. Prefer authored envelopes; keep generic simplification for irregular sources.
* **Single flat polygon per wall at all distances:** cheap but loses reveals, brick silhouettes and RT visibility. Preserve structural depth, important relief and close-range detail.
* **More threads alone:** cannot remove repeated whole-manifest interpretation, cubic empty-space work, synchronous GPU waits, or publication bottlenecks.
* **Just lower detail/disable LOD:** unacceptable as the main strategy. The result must get faster while preserving or improving observable detail.

## External technical references

* [meshoptimizer documentation](https://meshoptimizer.org/) describes attribute-aware simplification and vertex protection; useful for irregular mesh fallback, not a reason to simplify known wall shells from brick soup.
* [AMD Brixelizer documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer/) describes sparse distance-field infrastructure. Its geometry-to-field direction differs from our field-to-mesh requirement; borrow bounded sparse processing ideas, not an assumed drop-in implementation.
* [MoonRay shadow terminator guidance](https://docs.openmoonray.org/user-reference/how-to-guides/shadow-terminators/) explains the mismatch between smooth shading and coarse geometric visibility. This motivates retaining adequate structural geometry and validating ray offsets and shadows with normal maps.

These sources inform the design; the repository's live code and native measurements determine implementation decisions.
