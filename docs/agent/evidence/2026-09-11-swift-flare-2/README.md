# swift-flare.2 local-light raster evidence

Captured on 2026-09-11 with the canonical RelWithDebInfo MSVC editor on an
NVIDIA RTX 4090. Both timelines explicitly begin with `render_path raster`.
The launch exported the native variables through `WSLENV`; the first timeline
also used `MATTER_VK_VALIDATION=1` and `MATTER_TEST_RESIZE=1`.

## Results

- `LocalLightGallery`: 291 lights, 236 occupied cells, 512 GPU buckets,
  22,208 index bytes, and 11 worst-case candidates. The normal capture shows
  the 17x17 finite-radius point grid and local highlights on gold/chrome.
- Reload: the same 291-light publication was rebuilt and the post-reload shot
  completed without validation or lifetime errors.
- Settled reload: two frames captured 120 rendered frames apart before reload,
  then two more after reload with 120 frames of settling, are bit-identical.
  Pre/pre, pre/post, and post/post comparisons each report zero differing
  pixels and maximum channel delta zero. The earlier metal-only difference was
  therefore transient material residency, not local-light reload state.
- Point isolation: two point lights produced independent finite-radius pools
  and colored GGX highlights with sun and sky suppressed.
- Spot isolation: two spots produced bounded, feathered cones and local
  highlights with sun and sky suppressed.
- Empty publication: switching from the gallery to `LightingGarden` published
  zero lights/cells/buckets and drew successfully; switching back restored all
  291 lights. This exercises nonempty -> empty -> nonempty buffer replacement.
- Candidate debug: debug-view index 7 rendered the sparse cell candidate heat
  map for the multi-bucket gallery.

The validation run emitted the machine's existing missing EOS-overlay JSON
diagnostics and unused-interface warnings; it reported no Vulkan validation
error or VUID attributable to local-light descriptors, resource lifetime,
reload, or resize.

Raw stdout is retained in `native-validation.log` for the validation/resize/
reload/point/spot timeline and `native-zero-debug.log` for the candidate-view
and nonempty -> empty -> nonempty timeline. `native-settled-reload.log` records
the four 120-frame-settled reload captures driven by
`settled-reload-timeline.txt`.

## Compiled ABI check

`composite.frag` was compiled with Vulkan SDK 1.4.357 `glslc`, then disassembled
with `spirv-dis`. The relevant decorations are:

```text
OpMemberDecorate %_struct_278 0 Offset 0
OpMemberDecorate %_struct_278 1 Offset 12
OpMemberDecorate %_struct_278 2 Offset 16
OpMemberDecorate %_struct_278 3 Offset 20
OpMemberDecorate %_struct_278 4 Offset 24
OpMemberDecorate %_struct_278 5 Offset 28
OpDecorate %_runtimearr__struct_278 ArrayStride 32
OpDecorate %_runtimearr__struct_518 ArrayStride 64
```

The 32-byte cell stride matches `world_lights::LocalLightCell`; the 64-byte
record stride matches `world_lights::LocalLight`.
