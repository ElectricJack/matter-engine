# Native water, temporal and DLSS source ownership

`clear-crest-48.4.9` moves the production implementation from reference commit
`6490dbab806876631465259cdf5b397f258c5837`. It preserves behavior and existing
namespaces, feature macros, resource lifetimes, shader ABI and test oracles.

Accounting against the reference: 7,848 existing lines relocated (5,506 in
eight C++ pairs, 1,316 in eleven renderer fragments, 1,016 shader lines, and
10 enum/comment/declaration lines). Excluding documentation, `git diff --numstat`
adds 111 adapter/build lines beyond those moves; no algorithms are newly written.
Every new source file over 50 lines contains moved code. This longer document
records the retained boundaries and reproducible validation evidence.

| Moved implementation | Retained input/host boundary |
| --- | --- |
| `water_field_vk.*`, `water_field_vk_resources.*` | Prepared `WaterVisualProducts`/surface definitions become packed fields and native textures. Device allocation and submission remain supplied by the native host. |
| `water_animation_gpu.*`, `water_mesh_animation_playback.*` | Existing immutable hydrology animation artifacts and playback frames; decoding/cache ownership is unchanged. |
| `water_surface_reference.*`, `water_forward_reference.*` | Existing CPU water sampling, optics and compositing references. |
| `vk_temporal.*` | Explicit camera matrices, extents, instance transforms, tokens and invalidation values. |
| `streamline_bridge.*` | Existing Vulkan handles and typed DLSS options/constants/resources. Its former world header include is replaced by `include/matter/graphics/dlss_mode.h`. |
| `../../shaders/water_{forward, surface, screen_space}.glsl` | Existing forward shader and sampling helpers; legacy shader entrypoints forward here, retaining the original GLSL version/extension directives. |

The legacy C++ locations are forwarding includes, so existing native build
manifests compile the moved implementation once. There is no parallel renderer
or new producer. `matter::DlssMode`, its enumerator values and name-function
declaration moved unchanged from `world_session.h`; that header forwards the type.

The following private fragments remain at their original lexical positions in
`VkSceneRenderer`; they borrow the same host state and are not standalone APIs:

| Fragment | Preserved responsibility |
| --- | --- |
| `record_water_forward.inl` | Raster/water ordering and forward water draw recording. |
| `water_forward_bytes.inl` | Water attachment byte accounting within `vk_scene_detail`. |
| `water_publication.inl` | Field and animation publication, retirement and test diagnostics. |
| `water_descriptors.inl` | Water constants and field/forward descriptor writes. |
| `water_part_binding.inl` | Part field binding and existing fault-injection inspection. |
| `create_water_forward_pipelines.inl` | Existing forward pipeline construction. |
| `temporal_frame.inl` | Motion/jitter inputs and existing temporal reset propagation. |
| `dlss_controls.inl`, `dlss_history_reset.inl` | Preset/settings, diagnostics and reset consumption. |
| `dlss_output.inl` | Per-slot output allocation and lifetime handling. |
| `record_dlss.inl` | SDK evaluation inside the existing display recording scope, including native fallback, barriers and output selection. |

P4's delivered portable animation products cover skeletal/rigid animation, and
P6's delivered compute component covers solid assets. Neither supplies the native
water animation playback contract. This move retains the existing typed water
artifacts and their codec; it does not fabricate a producer conversion. World
coordination remains with ordered integration. Native device/header/renderer
coupling is retained and must not be counted as standalone graphics acceptance.

## Build and reproduce

From native PowerShell at the repository root:

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -NativeWaterTemporal -Test
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_editor
$env:VK_LAYER_ENABLES = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
Push-Location MatterEditor/build/cmake/windows-msvc/relwithdebinfo
foreach ($mode in 'water-field','water-field-nort','water-animation','water-forward','cull') {
    $env:MATTER_VK_SMOKE_MODE = $mode
    ./vulkan_smoke_tests.exe
    if ($LASTEXITCODE -ne 0) { Write-Error "Fixture failed: $mode" }
}
Pop-Location
```

The local `src/native/CMakeLists.txt` builds the eight moved implementation files
into a compatibility archive and reuses four existing engine fixtures. Only the
playback fixture links the existing artifact codec's source and its mesh-digest
support object (the fixture-support archive is linked to all four executables,
but unused members are not pulled in). No algorithm or fixture is copied. The
generated header check compiles the bridge without the former world include.
`-NativeWaterTemporal` selects `build/native-wt/relwithdebinfo`; the existing
independent facade/compositor build remains the wrapper's default.
Before linking this archive into a legacy renderer target, ordered integration
must remove that target's eight forwarding `.cpp` entries to avoid duplicate
definitions. Native device/resource definitions and artifact codecs remain host
link requirements; this task does not substitute an independent device API.

## Verification on 2026-10-11

Native MSVC 19.44.35211, Vulkan SDK 1.4.357.0, NVIDIA GeForce RTX 4090,
driver 610.74, device Vulkan 1.4.341. Editor and smoke builds passed. All four
local CTests passed: `water_field_vk`, `water_surface_reference`,
`water_forward_reference`, `water_mesh_animation_playback`.

The native `water-field`, `water-field-nort`, `water-animation` and
`water-forward` modes passed with synchronization validation enabled and zero
validation errors, including teardown. Existing oracles check immutable field
uploads and release, animation draw/fallback/retirement, surface optics and
material identity. The overlapping dry/wet material fixture measured exactly
`0.00000` HDR delta. The common preamble covers temporal reset/camera/extent
changes and bridge available/unavailable contracts. SDK behavior uses the
existing fake evaluator; the native build has `MATTER_ENABLE_STREAMLINE=OFF`.
Real proprietary SDK evaluation was not exercised.

The default facade/compositor wrapper also passes all five existing CTests,
including native Vulkan and the standalone dependency audit. The separately
built exact pre-move reference has identical native configuration options;
all 82 generated SPIR-V outputs compare byte for byte with the candidate.

The `cull` synchronization-validation run fails with four `WRITE_AFTER_WRITE`
hazards: two buffer-copy hazards in static upload/growth and two image-clear
hazards in the fake-DLSS evaluator. Its only two failed assertions reject errors
before/through teardown; functional assertions pass. One supervisor-authorized
comparison on a clean detached checkout of exact `6490dbab` reproduced the same
four complete hazard messages and both assertions under identical layer settings
and toolchain (normalizing only Vulkan handle addresses). Both cull runs exit 1.
The comparison predicate passed in 237.60 seconds; it establishes a pre-existing
failure and does not turn either cull mode into a clean validation pass.
Full candidate/baseline logs, layer configuration, comparison harness and shader
hashes are retained at `/tmp/clear-crest-48.4.9-evidence/` and linked in the task.

Byte comparison against the reference confirms all moved C++ pairs (normalizing
only the bridge include), all shader bodies, and exact expansion of the eleven
new renderer includes. The legacy shader version remains at its entrypoint.
The expanded renderer is 1,039,135 bytes, SHA-256
`8b8974e011db2ce3438da17ac8febddf93accb6c08b8fddf996f66e804fd5b9f`.
The rollback Make dependency database includes all three new shader paths for
82 concrete SPIR-V targets, and a dry run preserves the original entrypoint as
the compiler input. Root CMake's existing depfiles already track the includes.
