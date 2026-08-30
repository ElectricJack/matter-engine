# Water-animation memory admission

Date: 2026-08-30. Status: implementation direction under the authorized roadmap; not implemented or accepted by this document.

## Scope and authority

Close only the memory-enforcement item in [ROADMAP.md](../../../ROADMAP.md), preserving the existing limits specified by Task 12 of the [section-continuity plan](../plans/2026-08-29-animated-water-section-continuity.md). This work is independent of rejected waterfall refinement. It changes no fluid, water mesh, material, collision, or character behavior.

The hard limits are 1,073,741,824 bytes per complete `.mhwa` file, including its 32-byte outer header, and 734,003,200 bytes for all section and handoff animation files admitted for one network. They are not limits on peak process memory or decoded heap residency. Boundary particle sidecars remain build inputs and are reported separately.

## Existing behavior and gap

The artifact reader/writer already enforce a private complete-file ceiling. Playback sums complete file sizes against its caller's budget, and the engine currently supplies 700 MiB. Ready manifest publication and cached Ready admission do not enforce the aggregate ceiling. Provider timing fields can substitute raw frame bytes or zero after a failed file-size query; those values cannot establish admission or memory acceptance.

Centralize the existing constants and allocation-free checked arithmetic in the artifact module. Use C++17 pointer/count interfaces, not `std::span`. Preserve output arguments on rejected size calculations. A network calculation must reject an invalid individual file size, arithmetic overflow, or a total above the hard ceiling. A caller may request a lower playback budget but may not raise the hard ceiling.

## Admission and accounting

1. Count the complete serialized envelope, not just packed vertex/index bytes. The artifact serializer is the authority for its metadata, identity, frame directory, and payload layout. Exact-boundary arithmetic tests use numeric counts, not gigabyte buffers.
2. In the existing shared Ready animation-package validation, check file sizes for every section and handoff before decoding any of them. A missing/stat-failed/truncated/over-limit file fails closed. Only after aggregate admission may existing digest, identity, and frame validation proceed. Both immutable manifest publication and validated cache loading already pass through this seam and must continue to do so.
3. Playback independently applies the same hard limits before loading its candidate and also honors any lower caller budget. A failure preserves the active playback object. Keep its current separately measured retained/peak activation memory semantics.
4. Provider diagnostics report actual complete file sizes. A failed size query is an explicit product failure, never a raw-frame or zero substitute. Existing immutable files may remain after a failed candidate; no Ready manifest or active playback is replaced.
5. The continuity checker rejects missing, boolean, noninteger, nonpositive, over-limit, or inconsistent file counts. It checks both cold and cached traces and compares their admitted bytes. Sidecar and peak-build counts do not enter the runtime-file sum.

No file format, cache key, authored DSL cap, global memory manager, renderer allocation path, or RT eligibility policy changes are needed.

## Proof and completion

Use direct exact-limit/one-over/overflow helper tests, a small real serialization-size test, real package publication/load tests preserving prior state, and playback lower-budget/failure tests. Over-budget package tests may use sparse files whose logical sizes exceed the network budget; they must not allocate/decode gigabyte buffers. Assert the budget error occurs before payload validation, then remove only the uniquely owned temporary fixture.

Run native PhysX-enabled MSVC focused tests and Python comparator tests. Record exact commands, exits, test scope, and implementation commits in a findings document. These checks can close the memory item only. Final water appearance, waterfall/plunge quality, cold-bake/cache-hit visuals, and 1/10/16-shadow performance acceptance remain open.
