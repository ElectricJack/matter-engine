// MatterEditor/src/asset_root.cpp
//
// Implementation of asset_root.h -- the asset-root search order shared by
// examples_root(), issues_root() and shared_lib_root() in main.cpp.
//
// Dependency-free by design: <filesystem> plus the platform call that names
// this executable, so the test can link it alone.
#include "asset_root.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace viewer::assets {

// Directory holding this executable, or empty if it cannot be determined
// (GetModuleFileNameW failing or truncating at MAX_PATH, /proc/self/exe
// unreadable).
std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf, buf + n).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p.parent_path();
#endif
}

std::string resolve_asset_root_from(const char* name,
                                    const std::filesystem::path& exe_dir,
                                    const std::filesystem::path& cwd) {
    namespace fs = std::filesystem;

    auto walk_up = [&](fs::path dir) -> std::string {
        for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
            std::error_code ec;
            const fs::path candidate = dir / name;
            if (fs::is_directory(candidate, ec))
                return candidate.string();
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;   // reached the filesystem root
            dir = parent;
        }
        return {};
    };

    // Locate an asset directory by NAME rather than by a fixed number of "../".
    //
    // Layouts that have to work, and the depths they sit at:
    //   dev, launched from MatterEditor/      -> ../projects
    //   dev, launched beside the binary       -> ../../../projects (build/windows/)
    //   a packaged build (`make dist`)        -> ./projects        (next to the exe)
    //
    // Hard-coding "../" for one breaks the others -- which is exactly what
    // happened when the binary moved from MatterEditor/ into
    // MatterEditor/build/windows/.
    //
    // Four steps, in this order:
    //   1. <exe dir>/<name>          -- the only step a package needs, so a
    //                                   packaged build never walks and never
    //                                   sees a stray checkout above itself
    //   2. walk up from the cwd      -- THE AUTHORITATIVE step for a dev build
    //   3. walk up from the exe dir  -- a dev build launched from an unrelated
    //                                   cwd still finds the tree it was built in
    //   4. the bare name             -- keeps the old relative string so the
    //                                   failure message stays familiar
    //
    // Why the cwd outranks the exe walk-up (steps 2 before 3): automation
    // drives ONE shared editor binary with the working directory set to the
    // author's checkout, because that is what selects the tree under test.
    // Walking up from the executable first therefore resolved the checkout the
    // binary happened to be BUILT in and silently rendered that one's
    // projects/ instead of the edits under test. Step 1 stays ahead of step 2
    // so `make dist` keeps winning over anything above it.
    //
    // Step 1 keeps the error_code overload: an unreadable exe-adjacent path
    // must fall through to the next step, not throw out of a resolver whose
    // documented failure mode is "return the bare name".
    std::error_code ec;
    if (!exe_dir.empty() && fs::is_directory(exe_dir / name, ec))
        return (exe_dir / name).string();

    if (!cwd.empty())
        if (std::string hit = walk_up(cwd); !hit.empty()) return hit;

    if (!exe_dir.empty())
        if (std::string hit = walk_up(exe_dir); !hit.empty()) return hit;

    return name;   // preserve the old string so the failure message stays familiar
}

std::string resolve_asset_root(const char* name) {
    std::error_code ec;
    return resolve_asset_root_from(name, executable_dir(),
                                   std::filesystem::current_path(ec));
}

} // namespace viewer::assets