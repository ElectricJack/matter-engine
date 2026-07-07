// tileset_meadow_manifest_tests.cpp — verify Meadow's world.manifest declares
// ForestFloor as a tileset root. Non-GL test (parses only; no bake).

#include "part_graph.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_tests = 0, g_failures = 0;
#define REQUIRE(cond) do { \
    ++g_tests; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

int main() {
    const std::string world_data_dir = "../examples/world_demo/WorldData";
    std::vector<part_graph::ChildRequest> roots;
    std::string err;
    std::vector<bool> expand, tileset_flags;
    bool ok = part_graph::PartGraph::read_manifest(
        world_data_dir, "Meadow", roots, err, &expand, &tileset_flags);
    if (!ok) std::fprintf(stderr, "  read_manifest err: %s\n", err.c_str());
    REQUIRE(ok);

    bool saw_forest_floor_tileset = false;
    for (size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].module == "ForestFloor") {
            REQUIRE(tileset_flags[i]);
            REQUIRE(!expand[i]);
            saw_forest_floor_tileset = true;
        }
    }
    REQUIRE(saw_forest_floor_tileset);

    std::fprintf(stderr, "tileset_meadow_manifest_tests: %d run, %d failed\n",
                 g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
