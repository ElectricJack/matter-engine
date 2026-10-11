# Asset processing, persistence and format adapters

The persistence and format adapters consume immutable
`matter::services::AssetProduct` values and link only `matter_contracts`.
Optional surface producers generate material products from explicit inputs.
They neither generate geometry nor call a renderer, script host, live world,
or implicit resolver. Native integration registers them
through the injected cache/loader interfaces; central manifests and existing
Matter export entrypoints remain integration-owned.

| Local target | Behavior | Independent switch |
| --- | --- | --- |
| `matter_asset_archive` | Versioned prepared document and owned resource bytes; SHA-256 integrity | Always available |
| `matter_asset_storage` | `CacheService` persistence and explicit atomic file export | `MATTER_ASSET_STORAGE` |
| `matter_asset_gltf` | Static glTF 2.0 and GLB import/export; PNG/JPEG input, PNG output | `MATTER_ASSET_GLTF` |
| `matter_asset_legacy` | Injected existing Matter artifact reader and legacy hash checks | `MATTER_ASSET_LEGACY` |
| `matter_asset_surface` | Portable sampled surface to PBR textures or native float products | `MATTER_ASSET_SURFACE` |
| `matter_asset_surface_native` | Existing CPU local surface tape evaluation and baking | `MATTER_ASSET_SURFACE_NATIVE` |
| `matter_asset_pipeline` | Convenience interface target for enabled adapters | No geometry/graphics dependency |

All switches default ON. Turning them OFF removes their implementations from
the link graph. Geometry and graphics build independently without registering
any AssetPipeline target. The archive-only build has no filesystem/image/backend
dependency. No package publishing or installed service is involved.

The surface target links only contracts. Its optional native producer compiles
the existing `MatterEngine3/src/terrain_field.cpp` source directly in
`matter_asset_surface_runtime`, as TerrainGeneration does. Noise, tape parsing,
source evaluation and appearance evaluation retain that single implementation.
The old source location is temporary; relocation and compatibility entrypoint
wiring belong to ordered integration. The private height-overlay declaration
uses neutral hydrology/bounds headers; no concrete hydrology implementation,
terrain library, renderer, QuickJS or GPU dependency is linked. Disable both
surface switches for archive-only builds; the native switch requires the surface
switch and defaults to its value in a fresh configuration.

## Surface and material producers

`bake_surface(job, SurfaceImage, options, control)` converts an explicitly supplied
sample raster. `bake_surface_tape` additionally evaluates an authored local direct
source on supplied position/normal/tangent/footprint hits, using the existing CPU
`SurfaceRuntime`. Neither rasterizes geometry, resolves a material registry,
reads a world field, schedules work, writes a cache, compiles shaders nor manages
virtual textures. The coordinator supplies hits in the mesh's UV raster order.
VT residency, filtering policy, shader material implementation and scene upload
remain consumer responsibilities.

Inputs/outputs are owned values with the common job envelope. Inputs live through
the synchronous call; result bytes remain valid after the input is released.
Cancellation, malformed input, callback exceptions, budgets and unsupported
capabilities publish no product. The coordinator uses `services::admit` to reject
stale envelopes. Required processing capabilities and `options.algorithms` accept
only version 1 of `matter.processing.surface.bake` and, for the native producer,
`matter.processing.surface.local-tape`.

`SurfaceTexel` colors are linear; ORM means occlusion, perceptual roughness,
metallic. Normals are unit tangent-space vectors with +Y bitangent; height is in
metres. Coverage is binary. Rows are stored in supplied order, starting at v=0;
there is no vertical flip. The native producer validates orthonormal hit frames
and explicit tangent handedness. A normal perturbation uses the same local finite
differences as native face baking (`max(footprint/2, 0.0001m)`), then transforms
into the supplied tangent frame. Appearance order is tint, roughness bias,
wetness, metallic override, thin coating; coating blends squared roughness.

| Output | Material meaning and conversion policy |
| --- | --- |
| `pbr_rgba8` | Four owned RGBA8 textures: sRGB base color and emissive, linear metallic/roughness (G=roughness, B=metallic), linear tangent normal (`n*0.5+0.5`). Factors are neutral multipliers; alpha is 1. Sampler settings are explicit. Channels use nearest 8-bit quantization after sRGB encoding where appropriate. |
| `native_float32` | `SurfaceProduct::native` is a typed `NativeSurfaceDescription`; the same description is serialized as a **required** `matter.material.surface.samples` v1 extension. Raw linear color/ORM/normal/emissive are f32x3; relief is f32 metres; coverage is u8. No clamping or quantization of these resources occurs. |

Conventional conversion refuses any AO below 1, nonzero height, coverage holes,
or color/emissive above 1. A declared nonzero source height range is refused even
if sampled hits happen to have zero height. There is no silent flattening,
occlusion-to-color multiplication, alpha reinterpretation or HDR clipping.
The caller can request native output instead, explicitly adding
`{"matter.material.surface.samples", 1}` to
`options.validation.supported_capabilities`. This permission means the caller
can retain/consume those semantics; it does not add runtime support to a renderer.
Default contract consumers and the glTF adapter reject these native products.

The native extension fields exactly mirror the typed descriptor: version,
width/height, material ID, the six resource IDs, height min/max in metres, optional
source-tape resource ID, source version, and wrap/filter settings. Float resource
scalars are little-endian. The tape producer retains the exact source bytes and
declared relief range; the general sample converter uses source version 0 and
no tape. Every resource has an explicit shape and SHA-256. PBR fields on a native
product identify the material only; the required extension supplies its appearance.
A consumer may use the typed header without linking or calling a producer.
Prepared archives preserve the extension and bytes exactly when the consumer
opts in. Archive preservation is not execution of an advanced material.

The native producer supports receiver-independent local **direct-source v1**
tapes, including seeded noise/cellular operations, appearance modifiers and
relief. World/field inputs, receiver-material inputs, v2 receiver-dependent height
derivatives, and legacy material-classifier blending return explicit unsupported
diagnostics. Their original native paths remain available for integration to
preserve; this producer does not reinterpret them as a direct source. Strict
boundary parsing rejects extra tokens, nonfinite/prefix literals and unknown
operations. Limits include 64 KiB tape text, 1–16 noise octaves, safe int32 noise
lattices, local positions within +/-1,000,000m, relief within +/-1,000m, and positive
footprints up to 1m. Relief probes reject float coordinates too coarse for their
spacing. Sample count, byte storage and conservative tape work are checked against
job budgets before evaluation. Cancellation checkpoints bound raster loops.

## Prepared cache

`FileCache` accepts only `matter.portable.asset.v1` SHA-256 recipe keys. The build
coordinator owns the transitive source/parameter/seed/version/settings digest.
A cache key and an asset's semantic ID remain distinct. Each entry stores its
recipe digest followed by a version-1 `MAPA` archive: magic, version, bounded
body length, 64 lower-case SHA-256 characters, canonical document length/JSON,
resource count, and length-prefixed payloads in document resource order. Integers
are little-endian uint32. The checksum covers the complete document/payload body;
resource hashes and contract schema versions are validated independently.

Missing entries are successful null lookups. Corrupt entries, misplaced recipe
keys and unsupported versions return diagnostics, rather than cache misses.
`store_result(expected, key, result)` rejects failed/cancelled/stale results by
request ID, revision and generation epoch. A direct `store` is for already admitted
products. Neither method can accept a null/invalid product. Validation and encoding
precede filesystem work; a private sibling stage is flushed before the final
checkpoint and atomic replacement. Cancellation or callback/I/O failure before
commit preserves the previous entry and removes the stage. Callbacks run without
service locks. Instances are thread-confined, as specified by the service contract.
Completed stores are atomic to readers; parent-directory durability across power
loss is platform-dependent and is not promised.

`write_new_file` explicitly publishes encoded GLB/prepared bytes to a caller-selected
file and atomically refuses an existing destination. Its parent directory must
already exist. glTF returns JSON plus `asset.bin` bytes; callers own packaging and
URI resolution. No evaluation implicitly writes files.

Archive encoding preserves all contract fields, semantic IDs, shared prototypes,
assemblies, placements, materials and owned payloads exactly. Optional typed
products require explicit `ValidationOptions.supported_capabilities` opt-in;
unknown required capabilities fail. Nested resolved dependency assets are rejected:
prepared output must be self-contained, with exactly its declared payloads.

## glTF profile and material semantics

The adapter follows [Khronos glTF 2.0](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/Specification.adoc).
It supports triangle meshes, float32 positions/normals/tangents and up to eight
contiguous float32 UV sets, uint8/16/32 indices or nonindexed triangles, shared mesh
prototypes, one scene, parent trees, matrix or TRS placement, and opaque metallic/
roughness PBR. Interleaved attributes are range/alignment checked. Normals and
tangent directions must be unit length within `1e-4` squared-length error; geometry
and transforms are otherwise preserved rather than normalized. Matrices transpose
between glTF column-major storage and contract row-major storage; neither units
nor UVs are flipped. Mirrored and nonuniform placements remain explicit.

Base color and emissive textures require sRGB; normal and metallic/roughness
textures require linear data. glTF roughness uses G and metallic uses B. RGBA8
pixels, base-color factors and UV selection, normal scale, emissive factors,
repeat/clamp/mirror and nearest/linear sampling are retained. Data/normal/emissive
channels use UV set 0; base color can select another set. Unspecified glTF filters
map to linear. glTF's missing material/metallic factor defaults to metallic 1.
Texture uses in different color spaces get separate descriptors sharing their
decoded image resource. Image rows retain their encoded order. Embedded images
and explicitly supplied external image URI bytes are supported; the adapter does
not access the network/filesystem. Callers resolve data URIs too.

glTF has no shared assembly prototype object or stable semantic ID requirement.
Export writes ordinary shared meshes and expanded assembly node trees, plus an
owned checksummed prepared product in `extras.matterPrepared` referencing a buffer
view. Reimport regenerates and compares the complete standard scene JSON and binary
before restoring the original hierarchy, IDs, prototype sharing and metadata.
Changed standard data with stale preservation metadata fails explicitly. Consumers
can display the ordinary glTF without interpreting the extras; removing those
extras selects generic import with deterministic `gltf.*` IDs and node parents.
This preservation copy increases file size; no payload/performance target is claimed.
Mesh-only documents receive implicit root nodes for display on export.

Transparency, double-sided policy, occlusion textures, mipmapped samplers, HDR
emissive factors, vertex colors, normalized/sparse accessors, non-triangle topology,
noncontiguous UVs, mixed attribute sets across mesh primitives, morphs, rigs,
animation, cameras, multiple/unselected scenes and extensions return explicit
unsupported diagnostics. Optional Matter products/advanced surfaces require a
different adapter. These are declared boundaries, with no silent appearance loss.
Existing OBJ/MTL/advanced Matter exporters continue to use their native entrypoints;
this target does not replace their supported features or duplicate their parsers.

## Legacy compatibility boundary

`import_legacy` accepts artifact version 1 or 2 and the frozen `matter.source.v1`
or `matter.resolved.v1` legacy hash namespace. Integration injects the existing
Matter loader and conversion; absent loaders fail as unsupported. The boundary
validates the returned product, request envelope and exact uint64 provenance hash.
It never relabels an FNV identity as SHA-256 or invalidates an old cache. Actual
legacy artifact decoding/render-specific conversion remains with that loader;
the fixture tests this interface with an owned producer, not a production BLAS load.

## Focused verification

From native PowerShell, using the supported toolchain resolver:

```powershell
libs/AssetPipeline/build-windows.ps1 -Config RelWithDebInfo -Test -Matrix
```

From WSL:

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
  -NoProfile -ExecutionPolicy Bypass \
  -File "$(wslpath -w "$PWD/libs/AssetPipeline/build-windows.ps1")" \
  -Config RelWithDebInfo -Test -Matrix
```

Independent GNU/Linux checks:

```bash
cmake -S libs/AssetPipeline -B /tmp/matter-asset-pipeline -G Ninja \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/matter-asset-pipeline -j 2
ctest --test-dir /tmp/matter-asset-pipeline --output-on-failure -j 1
python3 libs/AssetPipeline/tests/independence.py \
  --build-root /tmp/matter-asset-independence
```

Nine CTests execute `AP-ARCHIVE/HASH/VERSION/CANCEL/BUDGET/CAPS`,
`AP-CACHE-REOPEN/NO-PARTIAL/STALE/ATOMIC/EXPORT`,
`AP-GLTF-ROUNDTRIP/GLB-ROUNDTRIP/SHARED/AFFINE/SCENE-MISMATCH`,
`AP-EXTERNAL/STRIDE/U16/NONINDEXED/TRS/PNG/SAMPLER/PBR`, malformed glTF/GLB, unsupported materials/color
spaces, and legacy version/namespace/hash fixtures. Assertions remain active in
Release. The matrix builds geometry, graphics and archive-only configurations,
plus portable-surface and native-surface configurations, then audits actual CMake
File API target graphs for absent storage/producer implementations. Surface cases
also inspect Ninja's compiler dependency closure for forbidden graphics/host headers.

Surface fixtures (`tests/surface_tests.cpp`) are
`AP-SURFACE-PBR/COLORSPACE/OWNER/NATIVE/NO-LOSS/CAPS/ARCHIVE`,
`AP-SURFACE-INVALID/BUDGET/CANCEL/STALE`, and
`AP-SURFACE-TAPE/APPEARANCE/RELIEF/NOISE/TAPE-INVALID/TAPE-UNSUPPORTED/TAPE-BUDGET`.
RGBA8 goldens and serialized payloads compare exactly. Float appearance and
analytic relief-normal checks use absolute `1e-6` (metres for height,
dimensionless otherwise). The native target also runs the existing
`MatterEngine3/tests/surface_field_tests.cpp` suite against the reused evaluator.
These establish CPU product semantics, not GPU rendered parity or live wiring.

To emit the textured repeated-assembly GLB for an independent glTF validator:

```bash
/tmp/matter-asset-pipeline/matter_asset_tests gltf /tmp/matter-asset-roundtrip.glb
```

Khronos glTF Validator reported zero errors and zero warnings for this fixture.
This establishes format validity and CPU round trips; rendered appearance parity,
live Matter wiring and performance remain integration work.
