# Task 2 Report: Immutable River Runtime Publication

## Outcome

Implemented the complete Task 2 slice on base `728bd1e11e0804130cd682642cd45d836271adfd` and committed it as `d9c4fb43` (`feat: publish immutable river runtime bindings`).

The implementation adds artifact v5 presentation persistence, network manifest v2 field digests and exact typed package closure, deterministic handoff presentation assembly, section-local presentation production, a provider/renderer-free public runtime binding, WorldSession access, and a single cancellation-safe publication record pairing the CPU binding with the existing render binding.

## Contract Decisions Applied

- Manifest v2 Ready closure contains exactly one Runtime and one Presentation typed field-product reference. Missing, duplicate, stale, or extra references are rejected.
- `RiverRuntimeBinding::generation()` is the accepted manifest `payload_digest`; runtime and presentation digests remain separate.
- Internal `hydrology::RiverFeature` conversion to public `matter::RiverFeature` is an exhaustive switch and rejects unknown values.
- The renderer and CPU binding share one immutable publication record and one atomic swap. Task 7 can extend that record without creating a second publication boundary.
- Candidate storage, render binding, and failed-debug binding are fully allocated before `hydrology_generation_mutex`; the locked region performs the stale-token check, provider commit, atomic swap, status update, and event emission.
- Failed candidates preserve the prior accepted publication. Cancelled candidates publish neither CPU nor renderer state. Failed-debug water remains visual-only and is installed only when no accepted publication exists.
- The temporary v4 three-key cache predicate was removed; v5 cache validation requires full `ProductKeys` equality including presentation.
- Per Ruling 9, the PhysX product path retains `GameplayFieldStatistics`, samples finite terrain records, and invokes the Task 1 presentation builder with neutral marker, wake, and local-override inputs.

## TDD Evidence

### RED

Tests were added before production implementation.

- `hydrology_artifact_tests` failed to compile because `HydrologyArtifact::presentation_field` did not exist.
- `hydrology_network_artifact_tests` failed to compile because manifest runtime/presentation digests, typed field references, and the field-product enum did not exist.
- `hydrology_handoff_products_tests` failed to compile because section/network presentation products did not exist.
- `async_bake_tests` failed to compile because `matter/river_runtime.h` did not exist.

The initial post-implementation focused run also usefully exposed:

- strict positive reconstructed normal-Y validation had temporarily been relaxed; the artifact negative fixture caught it and strict validation was restored;
- CTest did not inherit a Visual Studio developer PATH, so the dependency probe was made to discover and enter the MSVC environment explicitly; and
- the cancellation test originally waited without pumping queued GPU jobs, then stopped on generation A's expected cancellation event. It now pumps while approaching the publication barrier and waits through A cancellation for generation B's `BakeFinished`.

### GREEN

All builds used the required MSVC entry point.

Focused targets built successfully:

- `hydrology_artifact_tests`
- `hydrology_network_artifact_tests`
- `hydrology_handoff_products_tests`
- `river_runtime_tests`
- `async_bake_tests`
- `physx_dependency_contract_tests`
- `physx_adapter_contract_tests`

Renderer-conditioned compile verification:

- `tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_viewer_objects`
- Result: success; all 166 viewer product sources, including the renderer half of `matter_engine.cpp`, compiled.

Final focused CTest command:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R 'hydrology_artifact_tests|hydrology_network_artifact_tests|hydrology_handoff_products_tests|river_runtime_tests|async_bake_tests|physx_dependency_contract_tests|physx_adapter_contract_tests' --output-on-failure
```

Result: **7/7 passed**, 0 failed, 7.56 seconds.

Exactly one full CPU suite was run after focused GREEN:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -L cpu -j 8 --output-on-failure
```

Result: **37/37 passed**, 0 failed, 102.49 seconds real time.

## Changed Files

Public API and publication:

- `MatterEngine3/include/matter/river_runtime.h`
- `MatterEngine3/include/matter/world_session.h`
- `MatterEngine3/src/hydrology/river_runtime.cpp`
- `MatterEngine3/src/hydrology/river_runtime_internal.h`
- `MatterEngine3/src/matter_engine.cpp`

Persistence, products, and provider assembly:

- `MatterEngine3/src/hydrology/hydrology_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_artifact.cpp`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.h`
- `MatterEngine3/src/hydrology/hydrology_network_artifact.cpp`
- `MatterEngine3/src/hydrology/hydrology_handoff_products.h`
- `MatterEngine3/src/hydrology/hydrology_handoff_products.cpp`
- `MatterEngine3/src/hydrology/physx_fluid_bake.cpp`
- `MatterEngine3/src/hydrology/water_visual_products.h`
- `MatterEngine3/src/provider/local_provider.cpp`

Tests and dependency/build registration:

- `MatterEngine3/tests/river_runtime_tests.cpp`
- `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- `MatterEngine3/tests/hydrology_network_artifact_tests.cpp`
- `MatterEngine3/tests/hydrology_handoff_products_tests.cpp`
- `MatterEngine3/tests/async_bake_tests.cpp`
- `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- `MatterEngine3/tests/physx_adapter_contract_tests.cpp`
- `tools/tests/physx_dependency_contract_tests.ps1`
- `cmake/manifests/engine-core.sources`
- `cmake/MatterEngine.cmake`
- `cmake/MatterViewer.cmake`
- `cmake/tests/viewer_graph_tests.cmake`
- `MatterEngine3/tests/Makefile`

## Self-Review

- Artifact v5 writes exactly 21 bytes per gameplay record and 22 bytes per presentation record, validates exact section lengths and final remaining bytes, validates feature/wet bytes before conversion, and assigns only after complete digest and structural validation.
- Presentation persistence checks finite channels, `[0,1]` normalization, strict positive reconstructed normal Y, dimension equality, and gameplay/presentation wet-flag equality.
- Manifest v2 validates both nonzero field digests, exactly two distinct cache-relative typed references, exact digest correspondence, and canonical ordering. Deserialization is transactional.
- Handoff presentation uses gameplay-equivalent ownership selection, full positive-hemisphere normal reconstruction and renormalization, linear continuous channels, explicit nearest feature ownership, and rejects gameplay/presentation wet disagreement.
- Runtime construction recomputes both canonical field digests, rejects zero/mismatched metadata and layouts, deep-copies immutable storage, and performs exhaustive enum conversion. Sampling rejects empty, dry, non-finite, out-of-bounds, mismatched, or invalid storage. Batch null/count contracts are covered.
- Publication readers load one immutable record. No reader can combine a CPU generation with a renderer generation. Superseding a bake no longer clears the prior accepted record.
- The public header dependency probe rejects PhysX, Vulkan, Flecs, provider, render, and hydrology dependencies.
- Core/viewer hard-coded source censuses were updated and exercised by configuration plus the viewer object build.
- `git diff --check` passed before commit. Unrelated pre-existing untracked files were not staged or modified.

## Concerns

No known blockers or remaining Task 2 concerns. Task 6 is expected to replace the intentionally neutral presentation marker/wake/local-override inputs with authored values, and Task 7 is expected to extend the existing publication record rather than introduce a second swap boundary.
