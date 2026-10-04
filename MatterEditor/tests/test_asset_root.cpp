// Asset-root search order (MatterEditor/src/asset_root.cpp).
//
// The bug this pins: the resolver used to walk up from the EXECUTABLE's
// directory before the working directory, so one shared editor build -- run
// once per author worktree with the cwd set to that worktree's MatterEditor/
// -- rendered the projects/ of the checkout it happened to be built in instead
// of the edits under test. The order is now
//
//   1. <exe dir>/<name>          a packaged `make dist` layout still wins
//   2. walk up from the cwd      the checkout under test
//   3. walk up from the exe dir  a dev build launched from an unrelated cwd
//   4. the bare name             unchanged failure message
//
// resolve_asset_root_from() takes both roots as arguments, so every layout
// below is a directory tree this test builds under the temp dir rather than a
// process it has to fake.

#include "asset_root.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("  ok: %s\n", msg);                                \
        } else {                                                           \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

namespace fs = std::filesystem;

// mkdir -p of a directory whose existence is the whole point of a case: a
// projects/ folder only has to BE a directory for the resolver to find it.
fs::path make_dir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string resolve(const char* name, const fs::path& exe_dir, const fs::path& cwd) {
    return viewer::assets::resolve_asset_root_from(name, exe_dir, cwd);
}

fs::path root;

// Two checkouts: `built/` holds the editor that was compiled, `author/` the
// tree an automation run is pointed at. Both look like a dev tree, which is
// what made the old exe-first order silently render the wrong one.
void test_dev_layout_uses_the_cwd_checkout() {
    std::printf("-- a dev-layout exe resolves the cwd's projects/\n");
    const fs::path built_exe = make_dir(root / "built" / "MatterEditor" / "build" / "windows");
    make_dir(root / "built" / "projects");
    const fs::path author_editor = make_dir(root / "author" / "MatterEditor");
    const fs::path author_projects = make_dir(root / "author" / "projects");

    const std::string hit = resolve("projects", built_exe, author_editor);
    CHECK(hit == author_projects.string(),
          "the author's projects/ wins over the build checkout's");
    CHECK(hit != (root / "built" / "projects").string(),
          "the build checkout's projects/ is not returned");
}

void test_cwd_outranks_the_exe_walk_up_deep_in_the_tree() {
    std::printf("-- the cwd wins even when the exe sits deeper in its own tree\n");
    // build/windows-msvc/ is three levels under the built checkout's root, so
    // an exe-first search reaches built/projects before the cwd is consulted.
    const fs::path built_exe =
        make_dir(root / "deep-built" / "MatterEditor" / "build" / "windows-msvc");
    make_dir(root / "deep-built" / "projects");
    const fs::path author_editor = make_dir(root / "deep-author" / "MatterEditor" / "src");
    const fs::path author_projects = make_dir(root / "deep-author" / "projects");

    CHECK(resolve("projects", built_exe, author_editor) == author_projects.string(),
          "the cwd's projects/ wins over the deeper exe walk-up");
}

void test_packaged_layout_keeps_the_exe_adjacent_folder() {
    std::printf("-- a packaged layout still uses the folder next to the exe\n");
    // `make dist` root: editor.exe and projects/ side by side, launched from
    // the dist root. The cwd walk would agree here -- the case that actually
    // protects `make dist` is a cwd with no projects/ of its own but a
    // checkout above it.
    const fs::path dist = make_dir(root / "dist");
    const fs::path dist_projects = make_dir(dist / "projects");
    CHECK(resolve("projects", dist, dist) == dist_projects.string(),
          "an exe-adjacent projects/ is found");

    const fs::path packaged_exe = make_dir(root / "package" / "bin");
    const fs::path packaged_projects = make_dir(root / "package" / "bin" / "projects");
    make_dir(root / "package" / "projects");   // a stray checkout above the package
    const fs::path elsewhere = make_dir(root / "package" / "scratch" / "nested");
    CHECK(resolve("projects", packaged_exe, elsewhere) == packaged_projects.string(),
          "the exe-adjacent folder beats both cwd and exe walk-up");
}

void test_exe_walk_up_is_the_last_resort() {
    std::printf("-- a dev build launched from an unrelated cwd still finds its tree\n");
    const fs::path built_exe = make_dir(root / "lonely" / "MatterEditor" / "build" / "windows");
    const fs::path built_projects = make_dir(root / "lonely" / "projects");
    const fs::path elsewhere = make_dir(root / "unrelated" / "somewhere" / "deeper");

    CHECK(resolve("projects", built_exe, elsewhere) == built_projects.string(),
          "the walk-up from the exe dir runs when the cwd has no projects/");
}

void test_nested_and_sibling_resolvers_share_the_order() {
    std::printf("-- issues/ and MatterEngine3/shared-lib share the same order\n");
    const fs::path built_exe = make_dir(root / "multi" / "MatterEditor" / "build" / "windows");
    make_dir(root / "multi" / "issues");
    const fs::path author = make_dir(root / "multi-author");
    const fs::path author_issues = make_dir(author / "issues");
    const fs::path author_shared = make_dir(author / "MatterEngine3" / "shared-lib");

    CHECK(resolve("issues", built_exe, author) == author_issues.string(),
          "issues_root()'s lookup follows the cwd before the exe tree");
    CHECK(resolve("MatterEngine3/shared-lib", built_exe, author) == author_shared.string(),
          "shared_lib_root()'s nested lookup walks up to the cwd's engine root");
}

void test_missing_everything_returns_the_bare_name() {
    std::printf("-- nothing found keeps the bare relative name\n");
    const fs::path exe_dir = make_dir(root / "empty-exe");
    const fs::path cwd = make_dir(root / "empty-cwd");
    CHECK(resolve("projects", exe_dir, cwd) == "projects",
          "the failure message still says just \"projects\"");
    CHECK(resolve("projects", {}, {}) == "projects",
          "an undeterminable exe dir and cwd do not throw");
}

}  // namespace

int main() {
    std::printf("test_asset_root\n");
    root = fs::temp_directory_path() / "matter_asset_root_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    test_dev_layout_uses_the_cwd_checkout();
    test_cwd_outranks_the_exe_walk_up_deep_in_the_tree();
    test_packaged_layout_keeps_the_exe_adjacent_folder();
    test_exe_walk_up_is_the_last_resort();
    test_nested_and_sibling_resolvers_share_the_order();
    test_missing_everything_returns_the_bare_name();

    fs::remove_all(root, ec);
    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}