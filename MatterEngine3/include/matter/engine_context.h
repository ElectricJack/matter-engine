#pragma once
#include <memory>
#include <string>

#include "matter/world_session.h"

namespace matter {

class VulkanDevice;

struct EngineDesc {
    // .part cache location (parts/<hash>.part). Required: EngineContext::create
    // fails loudly (returns nullptr + err) if left null/empty rather than
    // silently falling back to a relative "cache" default -- a relative
    // default here previously let bake artifacts land wherever the process
    // happened to be launched from. When set, the engine canonicalizes it to
    // an absolute path via std::filesystem::absolute before storing it.
    const char* cache_root = nullptr;
    const char* shader_dir = nullptr;  // nullptr = embedded (MATTER_SHADER_DIR env overrides)
    bool allow_gl_lt_46 = false;       // true only for the ray-traced fallback path
    VulkanDevice* render_device = nullptr; // non-owning; app owns window/device
};

class EngineContext {
public:
    // Requires a live GL context current on this thread (the app owns the
    // window). Fails with a GL-version error if GL < 4.6 unless
    // desc.allow_gl_lt_46. Returns nullptr + err on failure; no exceptions
    // cross the API boundary.
    static std::unique_ptr<EngineContext> create(const EngineDesc& desc,
                                                 std::string& err);
    ~EngineContext();

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
