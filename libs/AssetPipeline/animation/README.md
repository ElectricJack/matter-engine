# Optional animation asset preparation

`matter_animation_asset` is a C++17 library over MatterContracts/MathLib only.
It is opt-in: static AssetPipeline and ProceduralGeometry builds do not add it.
It owns validated animation descriptors, rigs, four-influence weights and clip
keys from `AssetDocument`. Geometry bytes and graphics resources belong to the
consumer. Rig indices and authored clip keys are preserved; no rig is generated.

`prepare` validates animation capabilities, rig topology, skin references,
normalized weights, clip times and transforms before publishing immutable owned
values with the caller's request/revision/epoch. Joint and vertex budgets and
cancellation fail without publishing a partial result. Native defaults allow
256 joints per rig; the native adapter also enforces the existing authoring limits.

`graphics_frame` accepts a complete evaluated pose for exactly the requested
asset envelope and presentation serial. Matrices use the frozen contract's
row-major storage, column vectors, right-handed Y-up metres and seconds:

* Skin position palette: `model * inverse_bind`; normal palette: inverse transpose.
* Rigid placement: `entity_world * joint_model * binding_offset`.
* Previous matrices use the previous entity and joint pose, retaining motion data.

The output holds CPU matrices and semantic IDs. The renderer uploads/transposes
them and resolves prototypes separately. No ECS, renderer/device, QuickJS,
generation object, callback or runtime service is retained. Consumers must keep
the `Prepared` document and its `Bindings` together; a document alone cannot
represent all native motion semantics. `supports` reports `unsupported_capability`
for animation glTF export. Browser pose consumption supports portable contract
animation only; native motion graphs/Ozz clips explicitly fail that capability
check. This component does not sample portable clips or implement browser animation.

## Native adapter

`matter_animation_native`, under
`MatterEngine3/src/component_adapters/animation/`, is explicitly optional.
`prepare_native` takes the existing value-only `AnimationBuild`, finalized
`BindingBake`, resolved identity and nonce. It calls the existing canonicalizer,
authoring validator, Ozz baker, runtime-section encoder and binding codec directly.
Final geometry ownership, rigs and segment selections are caller supplied;
there is no automatic rigging, script evaluation or filesystem publication.

The returned `NativeArtifact` owns authored values, canonical values and native
section bytes. Existing baked clip blobs remain authoritative. The original
sample rate, loops, markers, additive clips, graph, inputs, controllers, targets,
sockets, binding pivots and UNORM16 weights stay in the native artifact. Rigid-only
assets receive the native `__bind_rest` clip/default graph convention. Preparation
decodes temporarily to verify the cross-section/Ozz contract; that temporary
runtime allocation is destroyed before the immutable artifact is published.

`prepare_runtime` explicitly creates the separately owned existing runtime
definition and its Ozz storage. `prepare_graphics` validates native sections and
projects the rig, skin weights, rigid pieces and socket attachments onto supplied
mesh prototype IDs. Every LOD/piece requires a mapping; the caller must map each
mesh to the exact finalized vertex order used by the weight baker. Geometry
fingerprints remain in the native binding artifact. Rigid group members receive
distinct length-framed group/joint IDs, and attachments have a separate prefix.
This projection consumes
evaluated poses; it declares native motion requirements and does not pretend to
convert graphs or Ozz clips to portable tracks. Weight conversion is UNORM16/65535.
The existing native source files are compiled in place, with no copies or symlinks.
Legacy native headers still include geometry/runtime declarations; no corresponding
engine, world, renderer or ScriptHost implementation is linked.

Central manifest and live-world integration are reserved for task `.9.4`.

## Build and verify

Supported Windows/MSVC wrappers use the repository's pinned toolchain resolver:

```powershell
.\libs\AssetPipeline\animation\build-windows.ps1 -Test -Matrix
.\libs\AssetPipeline\animation\build-windows.ps1 -Test -Native
```

Portable/Linux checks (Ninja, CMake 3.25+):

```sh
cmake -S libs/AssetPipeline/animation -B /tmp/matter-animation -G Ninja -DBUILD_TESTING=ON -DMATTER_ANIMATION_NATIVE=ON
cmake --build /tmp/matter-animation --parallel 3
ctest --test-dir /tmp/matter-animation --output-on-failure
python3 libs/AssetPipeline/animation/tests/independence.py --build-root /tmp/matter-animation-independence
```

Omit `MATTER_ANIMATION_NATIVE=ON` to verify a contracts-only build. The native
adapter directory can also be configured directly. Six tests cover:

| Fixture | Evidence |
| --- | --- |
| `ANIM-PORTABLE`, `ANIM-PIVOT`, `ANIM-BOUNDARY` | Clip/skin ownership, palette/normal/previous matrices, pivots, malformed inputs, budgets, cancellation, stale frames, capability errors |
| `ANIM-NATIVE`, `ANIM-WEIGHTS`, `ANIM-RIGID` | Canonical authored order, sampled Ozz clip, markers/rate/loop, exact weight recovery, rigid/socket transforms, explicit mapping, native limits, corruption, rest fallback |
| `ANIM-INDEPENDENCE-portable`, `ANIM-INDEPENDENCE-static` | Build/run portable animation and static generation separately; audit CMake File API targets/sources for forbidden dependencies |
| Existing `animation_ir_tests.cpp` | Native IR validation/canonicalization and authored limits |
| Existing `animation_binding_tests.cpp` | Indexed geometry signatures, selected segment weights, bindings, pivots, ownership and codecs |
| Existing `animation_ozz_adapter_tests.cpp` | Native skeleton/clip sampling, archive, blend and IK semantics |
| Existing `animation_runtime_asset_tests.cpp` | Seven-joint gait, graph/controller/marker semantics, runtime round trip and corruption rejection |
