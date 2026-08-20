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
    // NOTE: the name and the trailing comment below are both historical and
    // no longer describe what this flag does. Since the GL render path was
    // deleted it means "headless kernel: no interactive renderer required" —
    // EngineContext::create rejects a desc that has neither `render_device`
    // nor this flag, and every headless test suite sets it. There is no GL
    // version check left in create() at all. See the comment at the
    // corresponding check in MatterEngine3/src/matter_engine.cpp.
    bool allow_gl_lt_46 = false;       // true only for the ray-traced fallback path
    VulkanDevice* render_device = nullptr; // non-owning; app owns window/device
};

class EngineContext {
public:
    // Requires a live GL context current on this thread (the app owns the
    // window). Fails with a GL-version error if GL < 4.6 unless
    // desc.allow_gl_lt_46. Returns nullptr + err on failure; no exceptions
    // cross the API boundary.
    // CORRECTION: the paragraph above predates the Vulkan-only migration and
    // is no longer accurate — create() neither requires nor checks a GL
    // context, and there is no GL-version failure path. What it actually
    // does: registers the calling thread as the engine's render thread,
    // applies `desc.shader_dir`, requires a non-empty `desc.cache_root`
    // (canonicalized to an absolute path), and requires either a
    // `desc.render_device` or `desc.allow_gl_lt_46`. The "returns nullptr +
    // err, no exceptions cross the boundary" part still holds.
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
