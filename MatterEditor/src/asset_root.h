#pragma once
// MatterEditor/src/asset_root.h
//
// Locating the engine's asset directories -- projects/, issues/,
// MatterEngine3/shared-lib -- by NAME instead of a fixed number of "../".
//
// Extracted from main.cpp so the search order can be unit-tested without
// linking the editor's Vulkan/ImGui/Vulkan-loader world: the resolver is pure
// filesystem code, and tests/test_asset_root.cpp drives it with injected exe
// and working directories.
#include <filesystem>
#include <string>

namespace viewer::assets {

// Directory holding this executable, or empty if it cannot be determined
// (GetModuleFileNameW failing or truncating, /proc/self/exe unreadable).
std::filesystem::path executable_dir();

// The asset-root search itself, with both roots injected so a test can
// describe a layout without a real process at either end. Either path may be
// empty, which skips that branch; see the implementation for the order.
std::string resolve_asset_root_from(const char* name,
                                    const std::filesystem::path& exe_dir,
                                    const std::filesystem::path& cwd);

// resolve_asset_root_from(name, executable_dir(), current_path()) -- what
// examples_root(), issues_root() and shared_lib_root() call.
std::string resolve_asset_root(const char* name);

} // namespace viewer::assets