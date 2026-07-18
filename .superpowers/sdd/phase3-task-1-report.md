# Phase 3 Task 1 Report: Public Streaming Contract and Pure Streamer Terminology

## Implementation summary

- Added the data-only public `matter::streaming` contract in
  `MatterEngine3/include/matter/streaming.h`.
- Exposed the contract through `matter/ecs.h`, reflected all required enums and
  data members in `StreamingModule`, made `StreamingUpdate` depend on
  `matter::ecs::FrameUpdate`, and imported the module from `ecs_runtime::Runtime`.
- Added the requested ECS default-contract test and the clear/late-publish pure
  streamer test.
- Renamed `SectorStreamer` camera-specific parameters, members, locals, and
  comments to anchor terminology without changing its update signature,
  selection logic, defaults, hysteresis, cooldown, key packing, or requests.

## RED evidence

1. The tests were added before the public contract implementation. The targeted
   MSVC compile seam then failed as expected because the namespace and types did
   not exist:

   ```text
   MatterEngine3\\tests\\ecs_tests.cpp(1395): error C2653: 'streaming': is not a class or namespace name
   MatterEngine3\\tests\\ecs_tests.cpp(1395): error C2065: 'SectorStreaming': undeclared identifier
   ```

   Command:

   ```powershell
   $compile = 'call "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc /c MatterEngine3\\tests\\ecs_tests.cpp /FoNUL /IMatterEngine3\\include /IMatterEngine3\\src /IMatterEngine3\\src\\render /IMatterSurfaceLib\\include /IMatterSurfaceLib\\src /ILibraries\\flecs /ILibraries\\box3d\\include /IParticleFlowLib\\include /ILibraries\\Vulkan-Headers\\include'
   cmd /d /c $compile
   ```

2. The canonical focused GNU Make command is unavailable on this Windows host:

   ```powershell
   make -C MatterEngine3/tests run-ecs run-sectorstream
   ```

   ```text
   make : The term 'make' is not recognized as the name of a cmdlet, function, script file, or operable program.
   ```

## GREEN evidence

1. The same MSVC seam compiled both the ECS test source and changed runtime
   source successfully (exit 0):

   ```text
   ecs_tests.cpp
   ecs_runtime.cpp
   ```

2. The pure streamer test was compiled and run with MSVC (exit 0):

   ```text
   long flight: peak=7997 end=7993
   ALL PASS
   ```

3. `git diff --check` exited 0. The terminology scan
   `rg -n -i 'camera|\\bcam\\b|cam_' MatterEngine3/src/sector_streamer.h MatterEngine3/src/sector_streamer.cpp`
   produced no matches.

## Files changed

- `MatterEngine3/include/matter/streaming.h` (new)
- `MatterEngine3/include/matter/ecs.h`
- `MatterEngine3/src/ecs/ecs_runtime.cpp`
- `MatterEngine3/src/sector_streamer.h`
- `MatterEngine3/src/sector_streamer.cpp`
- `MatterEngine3/tests/ecs_tests.cpp`
- `MatterEngine3/tests/sector_streamer_tests.cpp`

## Self-review

- The public header contains only the exact requested data types and module
  declaration; it adds no policy, coordinator, serializer, or DSL surface.
- Reflection covers all three `SectorStreamingErrorCode` constants, all five
  `SectorStreamingState` constants, and every member of both reflected data
  structs.
- `StreamingUpdate` is the only added phase and depends directly on
  `ecs::FrameUpdate`.
- The `SectorStreamer` behavioral diff is limited to naming and comments;
  `update(float, float)` remains unchanged at the ABI level.

## Concerns

GNU Make is not installed, and the ECS target could therefore only be verified
through targeted MSVC static compilation rather than a fully linked/run ECS
binary. The pure CPU streamer executable was fully compiled and run successfully.
