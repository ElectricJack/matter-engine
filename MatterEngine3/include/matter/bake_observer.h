#pragma once

// BakeObserver — optional, Lab-only seam for watching a part bake rung-by-
// rung (Part Workbench W3, docs/part-workbench.md SS-I.4 "per-rung live
// watch"). Mirrors the discipline already established by bake_trace's
// thread-local collector (bake_trace.h) and by WorldSession::set_test_fault_hook:
// an optional pointer, default null, checked at every call site before use, so
// production bakes (which never set an observer) are byte-for-byte unaffected.
//
// THREAD CONTRACT (load-bearing — read before implementing an observer):
//   - on_mesh_ready fires from script_host::bake_source, on whatever thread
//     runs the bake (the install/bake worker thread for a normal async
//     request_bake(); the calling thread directly for a synchronous
//     bake_source() call, e.g. in tests).
//   - on_rung_ready fires from lod_bake::bake_lods(), on whatever thread
//     calls it. In production that is PartStore::get_or_load(), which today
//     runs INSIDE a GL-thread publish job (matter_async::assert_gl_thread) —
//     so, unlike on_mesh_ready, this callback may already be on the GL
//     thread. Treat both callbacks as "unknown thread, not necessarily GL"
//     and never assume otherwise: a caller that invokes bake_lods() from a
//     worker thread (as headless tests and any future off-GL-thread LOD bake
//     path may do) gets the same contract.
//   - The callee MUST NOT touch GL, ImGui, or any windowing/render state
//     directly from inside either callback, regardless of which thread it
//     happens to land on. Copy whatever data you need and marshal actual
//     GL/UI work through the session's existing GpuJobQueue (pump_gpu_jobs)
//     or event mechanism (poll_event) — never act on GL state synchronously
//     inside the callback.
//   - Any pointers/spans passed to a callback are valid only for the
//     duration of that call; copy anything you want to retain past return.
//   - A null BakeObserver* is the default at every call site down the bake
//     path (BakeOptions::observer, PartStore::set_bake_observer). Zero cost
//     when unset: each site does a plain pointer null-check before touching
//     the observer, so the null-observer path is byte-identical to the
//     pre-W3 code.
struct BakeObserver {
    virtual ~BakeObserver() = default;

    // Fired once per part bake, right after the full-resolution (LOD0) mesh
    // has been produced by build()/meshing in script_host::bake_source,
    // before the part is saved. `tris` is the triangle count of that mesh.
    // (The (void) casts keep the documented parameter names legal under
    // -Wextra -Werror translation units, e.g. the Vulkan smoke suite.)
    virtual void on_mesh_ready(int tris) { (void)tris; }

    // Fired once per LOD ladder level as lod_bake::bake_lods() finishes
    // decimating (or, for LOD0, registering unchanged) that level.
    // `level` is 0-based (0 = finest, matching LodLevels/BakeTargets order).
    // `tris` is the triangle count of the produced level's geometry.
    // `ms` is the wall-clock time spent producing this level (decimation
    // cost only, not including BLAS registration).
    virtual void on_rung_ready(int level, int tris, double ms) {
        (void)level; (void)tris; (void)ms;
    }
};
