# Exact VT material identity sampling

Date: 2026-09-15. Native Windows x64 MSVC, RelWithDebInfo. NVIDIA GeForce RTX 4090, driver 610.74, Vulkan 1.4.341 as reported by the test device.

## Result

The VT auxiliary channel stores two u8 material IDs and their blend weight in uncompressed RGBA8. The compositor already preserves those bytes; all runtime pool channels were nevertheless bound to a linear sampler. Rounding an interpolated ID between 30 and 34 could select an unrelated material 31, 32 or 33 for near detail.

`vt_sample_channel` now uses `texelFetch` for the auxiliary channel, keeping both IDs and the weight from one exact texel. Other channels retain their existing filtering. The physical page format, pool memory and source assets are unchanged. This completes the first current-path correctness step in [L1](../../../superpowers/plans/2026-09-14-layered-surface-texturing.md#l1--correct-current-material-sampling-and-frequency-ownership); it does not implement smooth reconstruction of neighboring material pairs or the shared layered evaluator.

## Native regression

The existing `vt-surfaces` mode now includes a real compositor/G-buffer fixture with a surface tape switching between IDs 30 and 34. Those two materials use the same flat detail source; unused IDs 31–33 initially use a strongly tilted normal source. A row-major instance translation moves the boundary through five subtexel positions at a fixed pixel. Editing only the unused materials must not change the visible normal or regenerate any pages. A positive control changes authored ID 30 and must change visible detail.

Before the sampler fix, all five boundary assertions failed with maximum normal-component delta **1.005054** when only unused materials changed. Afterward every delta is **0.000000**, the unused edit regenerates zero pages, and the authored edit still changes detail. The final red and green runs differ only in `vt_common.glsl` among their monitored source inputs.

| Run | Build exit | `vt-surfaces` exit | Boundary assertions | Vulkan validation errors |
| --- | ---: | ---: | --- | ---: |
| [Final red](categorical-red-boundary-checks.json) | 0 | 1 | Five expected failures | 0 |
| [Green](categorical-green-checks.json) | 0 | 0 | All pass | 0 |

Raw outputs: [red](categorical-red-boundary-test-vt-surfaces.log), [green](categorical-green-test-vt-surfaces.log). Source/binary identities: [red](categorical-red-boundary-source.json), [green](categorical-green-source.json). Exact commands and durations are in the check files.

### Fixture corrections retained in the history

The first attempt exposed an older fixed-five-frame startup assumption: its surface probes ran before asynchronous page preparation finished. The fixture now waits, with a bounded frame count, for actual page work to drain and remain settled. An intermediate boundary fixture also used the wrong matrix translation index and therefore did not exercise the interpolated boundary. Correcting it to the renderer's row-major transform produced the decisive five red failures above. The earlier logs are retained as diagnostic history and are not counted as proof of the sampler fix.

The mode is registered as native CTest `vt_surface_material_tests`. The existing tape, edit and determinism cases remain in the same mode.

## Integration and limits

Broader native validation is recorded in [integration checks](categorical-integration-checks.json) with [source/binary identity](categorical-integration-source.json). Both additional builds (`vt_compositor_tests`, `matter_editor`) exit 0. All five additional GPU modes exit 0: compositor, retained input snapshots, finished-surface POM, feedback and VT/RT sampling. Each reports zero Vulkan validation errors. Together with the green surface test, this is six passing GPU modes on the final source revision. No monitored source changes occurred during any build or test. [Final verification](final-verification.json) checks the current source and binary identities, document links, whitespace and the archived change patch.

This slice does not claim an overall generation or scene-performance improvement. Its wall-clock test durations include native startup and shader/pipeline initialization. POM still uses the established source path, and near shading still selects only the first material contributor and applies the existing detail overlay. Shared DSP/SDF material generation, coherent layered height, generated splats, terrain/brick visual proofs, secondary-contributor coverage and full VT acceptance remain open under the active visual-first goal.
