#pragma once

// MatterEngine3/include/matter/engine_context.h
//
// The engine's top-level entry point. `EngineContext` is the process-wide
// handle you create once; `WorldSession` (matter/world_session.h) is the
// per-world handle you open from it. Everything else in MatterEngine3 hangs
// off one of those two.
//
// Typical use (see MatterEditor/src/main.cpp):
//
//   matter::EngineDesc desc;
//   desc.cache_root    = "<project>/.cache";  // required — see below
//   desc.render_device = &vulkan_device;      // interactive sessions only
//   std::string err;
//   auto engine = matter::EngineContext::create(desc, err);  // nullptr = fail
//   auto session = engine->open_world(world_desc, err);
//
// Conventions and gotchas:
//   * No exceptions cross this API boundary. Every fallible call returns
//     nullptr/false and fills `err` with a human-readable reason.
//   * The context is non-copyable and owns its `Impl`. A WorldSession keeps a
//     raw pointer to that Impl, so the context must outlive every session
//     opened from it.
//   * The thread that calls create() is registered as the engine's render
//     thread, which arms the debug thread-affinity asserts. Call it from the
//     thread that owns the window and the Vulkan device.
//   * `Impl` is public only so the implementation file can name it; it is not
//     part of the API.

#include <memory>
#include <string>

#include "matter/world_session.h"

namespace matter {

class VulkanDevice;

// Creation parameters for EngineContext. Borrowed for the duration of
// create() only — `cache_root` is copied (and canonicalized to an absolute
// path) during the call, so the caller's buffer need not outlive it.
struct EngineDesc {
    // .part cache location (parts/<hash>.part). Required: EngineContext::create
    // fails loudly (returns nullptr + err) if left null/empty rather than
    // silently falling back to a relative "cache" default -- a relative
    // default here previously let bake artifacts land wherever the process
    // happened to be launched from. When set, the engine canonicalizes it to
    // an absolute path via std::filesystem::absolute before storing it.
    const char* cache_root = nullptr;
    const char* shader_dir = nullptr;  // nullptr = embedded (MATTER_SHADER_DIR env overrides)
    // "Headless kernel: no interactive renderer required."
    //
    // THE NAME IS HISTORICAL AND MISLEADING — it has nothing to do with GL or
    // with version 4.6 any more; the GL render path was deleted and create()
    // contains no version check of any kind. What the flag actually does is
    // waive the render_device requirement: create() rejects a desc that has
    // neither `render_device` nor this flag. Every headless test suite sets
    // it, and the bake pipeline null-checks render_device throughout, so a
    // deviceless context bakes but cannot draw. Renaming it (to something like
    // `headless`) is a mechanical but repo-wide change across the test suites
    // and has been deliberately deferred; see the matching check in
    // MatterEngine3/src/matter_engine.cpp.
    bool allow_gl_lt_46 = false;
    VulkanDevice* render_device = nullptr; // non-owning; app owns window/device
};

class EngineContext {
public:
    // Creates the process-wide context. In order, create():
    //   * registers the calling thread as the engine's render thread (arms the
    //     debug thread-affinity asserts) — so call it from the thread that
    //     owns the window and the Vulkan device;
    //   * applies `desc.shader_dir` (nullptr clears to env/embedded);
    //   * requires a non-empty `desc.cache_root`, canonicalized to an absolute
    //     path;
    //   * requires either `desc.render_device` or `desc.allow_gl_lt_46`.
    // There is no GL context requirement and no GL-version check — both were
    // removed with the GL render path. Returns nullptr + err on failure; no
    // exceptions cross the API boundary.
    static std::unique_ptr<EngineContext> create(const EngineDesc& desc,
                                                 std::string& err);
    ~EngineContext();

    // Opens one world against this context. The returned session holds a raw
    // pointer back into the context's Impl, so this context must outlive it.
    // Returns nullptr + err on failure.
    std::unique_ptr<WorldSession> open_world(const WorldDesc& desc,
                                             std::string& err);

    EngineContext(const EngineContext&) = delete;
    EngineContext& operator=(const EngineContext&) = delete;

    struct Impl;
private:
    explicit EngineContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace matter
