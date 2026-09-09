// test_sandbox.h — portable scratch-directory helpers for the MatterEngine3
// headless test suites.
//
// Several suites bake into a throwaway cache directory and chdir() into it
// (the bakers write the RELATIVE "parts/<hash>.part", so the writer and the
// cache check only agree when the process cwd is the cache root). They used to
// build that directory with
//
//     system(("rm -rf "   + sandbox).c_str());
//     system(("mkdir -p " + sandbox + "/parts").c_str());
//     chdir(sandbox.c_str());
//
// against a hardcoded "/tmp/<name>". That is a POSIX-shell assumption:
// system() runs cmd.exe on Windows, where neither `rm -rf` nor `mkdir -p`
// exists, so both commands printed "The syntax of the command is incorrect.",
// the directory was never created, the chdir failed, and EVERY assertion
// downstream of it failed. This header keeps the same semantics (wipe, then
// create the requested sub-directories) expressed with <filesystem>.
//
// Rooting mirrors part_graph_integration_tests.cpp's local_fixture_root():
// fixtures live under the test working directory, NOT under the system temp
// dir — in native MSYS2, temp_directory_path() follows TMP and can resolve to
// the MSYS installation's protected /tmp (C:\msys64\tmp), so a fixture can
// fail to create its cache root before it exercises anything at all.
//
// Callers that leave their sandbox behind (either deliberately, to show warm
// cache behaviour on a second run, or because they chdir'd into it and cannot
// remove their own cwd) should pass a name under "sandbox/", which
// .gitignore already reserves for exactly this — otherwise the leftovers show
// up as untracked files in the repo.
#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>

// Absolute path for a fixture directory `name`, resolved against the current
// working directory (the suites run from MatterEngine3/tests). Falls back to
// the relative name if the cwd cannot be read.
static inline std::filesystem::path local_fixture_root(const char* name) {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(name, ec);
    return ec ? std::filesystem::path(name) : root;
}

// Wipe and recreate <root>, plus each requested sub-directory. Returns the
// absolute root so callers can chdir() into it.
static inline std::string make_sandbox(const char* name,
                                       std::initializer_list<const char*> subdirs = {"parts"}) {
    const std::filesystem::path root = local_fixture_root(name);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    for (const char* sub : subdirs) std::filesystem::create_directories(root / sub, ec);
    return root.string();
}

// Same, but keeps whatever is already there (for suites whose point is that a
// second run hits a warm cache).
static inline std::string ensure_sandbox(const char* name,
                                         std::initializer_list<const char*> subdirs = {"parts"}) {
    const std::filesystem::path root = local_fixture_root(name);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    for (const char* sub : subdirs) std::filesystem::create_directories(root / sub, ec);
    return root.string();
}

static inline void destroy_sandbox(const std::string& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
