# Phase 3 Task 2 Report: Worker-Only Streaming Coordinator

## Status

Implemented the private, CPU-only `matter::streaming::detail::Coordinator` and
its focused behavioral suite. The coordinator owns the only `SectorStreamer`,
coalesces plain attachment/profile/anchor/restart intents behind one mutex,
queues acknowledgements as values, applies all streamer mutations on worker-owned
methods, and publishes only a copied status snapshot.

## RED evidence

The complete focused test was created before either production coordinator file
existed. The first MSVC compile failed for the expected missing feature, not for
a test syntax error:

```text
sector_streaming_coordinator_tests.cpp
MatterEngine3\tests\sector_streaming_coordinator_tests.cpp(2): fatal error C1083: Cannot open include file: '../src/streaming/sector_streaming_coordinator.h': No such file or directory
sector_streaming_coordinator.cpp
c1xx: fatal error C1083: Cannot open source file: 'MatterEngine3\src\streaming\sector_streaming_coordinator.cpp': No such file or directory
sector_streamer.cpp
Generating Code...
```

Command:

```powershell
$compile = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++17 /EHsc MatterEngine3\tests\sector_streaming_coordinator_tests.cpp MatterEngine3\src\streaming\sector_streaming_coordinator.cpp MatterEngine3\src\sector_streamer.cpp /Fe:.superpowers\sdd\sector_streaming_coordinator_tests.exe /IMatterEngine3\include /IMatterEngine3\src /ILibraries\flecs'
cmd /d /c $compile
```

After the production files compiled, the first link also confirmed that the
existing public `matter/streaming.h` contract requires the vendored Flecs C
object for its C++ constants. The focused MSVC and canonical Make links were
therefore closed over `flecs.c`, compiled as C17. The coordinator itself does
not create or call a Flecs world.

## GREEN evidence

The fresh focused MSVC C17/C++17 build and executable run exited 0:

```text
flecs.c
sector_streaming_coordinator_tests.cpp
sector_streaming_coordinator.cpp
sector_streamer.cpp
Generating Code...
ALL PASS
```

The test covers:

- unattached/profile-ready zero state and requests;
- independent profile and anchor readiness;
- synchronous profile copying (caller mutation after `set_profile`);
- duplicate owner rejection with first full generational owner preserved;
- newest-anchor-wins coalescing and tagged request allocation;
- ignored nonowner detach, immediate owner-detach invalidation, old-generation
  resident evictions, and rejected late acknowledgement;
- failed acknowledgement residency/cooldown/retry behavior;
- coalesced restart with one generation increment; and
- detach superseding an unprocessed restart.

The existing pure `SectorStreamer` was freshly compiled and run with MSVC:

```text
sector_streamer_tests.cpp
sector_streamer.cpp
Generating Code...
  long flight: peak=7997 end=7993
ALL PASS
```

The Phase 2 static checker exited 0:

```text
PASS: Box3D Phase 2 build contract
 - Runtime owns one opaque context after its Flecs world member
 - every Runtime source graph includes context, shapes, and systems exactly once
 - final test/application recipes consume exactly one selected Box3D archive
 - every public header is Box3D-opaque and Windows flattened basenames are unique
 - C17 C dependencies and independent standard physics gates are closed
```

`git diff --check` exited 0. A focused source scan found every direct
`SectorStreamer` mutation only in the private coordinator implementation: clear
and eviction collection helpers, `worker_step`, and worker-only `next_request`.

## Canonical build wiring

- `MatterEngine3/tests/Makefile` defines the focused coordinator source/object
  closure, links the vendored Flecs C17 object, exposes `run-sectorcoord`, and
  removes its binary during `clean`.
- `MatterEngine3/Makefile` adds the coordinator source and object to the static
  archive closure and adds `src/streaming` to C++ source lookup.

The required GNU Make command was attempted honestly:

```powershell
make -C MatterEngine3/tests run-sectorcoord
```

It is unavailable on this host:

```text
make : The term 'make' is not recognized as the name of a cmdlet, function,
script file, or operable program.
```

No GNU Make success is claimed.

## Files changed

- `MatterEngine3/src/streaming/sector_streaming_coordinator.h` (new)
- `MatterEngine3/src/streaming/sector_streaming_coordinator.cpp` (new)
- `MatterEngine3/tests/sector_streaming_coordinator_tests.cpp` (new)
- `MatterEngine3/tests/Makefile`
- `MatterEngine3/Makefile`
- `.superpowers/sdd/phase3-task-2-report.md` (new)

## Self-review

- The exact required public methods and value structs are present only in the
  private `src/streaming` header; no public ECS component gained behavior.
- One mutex protects all app-facing intent/ack values and copied snapshot
  exchange. No new thread, condition variable, ECS object, or streamer pointer
  crosses the boundary.
- `set_profile` copies before returning and null removes readiness. Anchor and
  repeated restart intents coalesce before a worker step.
- Issued requests are tracked by full owner, coordinator generation, and sector,
  so stale, fabricated, or duplicate acknowledgements cannot call the streamer.
- Detach sets the externally visible snapshot to detached/generation zero while
  holding the mutex. The next worker step clears the old streamer, tags all
  resulting evictions with the old owner/generation, and cannot publish a stale
  snapshot across a concurrent attachment revision.
- Generation zero is reserved; successful initial activation/restart allocates
  one monotonic nonzero generation. Multiple restart calls before a step collapse
  into one recreation.
- Failed acknowledgements use `on_failed`, do not increment residency, and leave
  the request retryable under the streamer's configured update cooldown.

## Concerns

GNU Make is unavailable, so `run-sectorcoord` and the full static archive were
not executed through their canonical GNU recipes. The exact source closure was
instead compiled and run with MSVC, the Make wiring was inspected, the existing
streamer executable passed, and the repository static build-contract checker
passed. Because the existing public streaming header includes the Flecs C++ API,
even this CPU-only test must link the vendored Flecs C17 object; it still performs
no ECS/world or GPU work.
