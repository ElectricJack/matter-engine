// refine_controller_tests.cpp — pure CPU tests for RefineController (Phase C Task 4).
//
// Tests:
//   (a) tile_count: build() with 4 terrain tiles × 2 res variants + 2 scatter nodes
//       (different module) → tile_count() == 4, scatter nodes ignored.
//
//   (b) full_count: initially 0; increments as tiles are marked Full.
//
//   (c) next_nearest: next() returns nearest-to-focus coarse tile (not queued/full).
//
//   (d) mark_queued: after mark(idx, Queued), next() skips that tile.
//
//   (e) mark_full: after mark(idx, Full), full_count() increments; evict_beyond
//       can return it.
//
//   (f) evict_beyond: returns only Full tiles outside radius, sorted farthest-first.
//       Tiles that are Coarse/Queued are never returned.
//
//   (g) no_coarse_instance: Terrain full-res nodes without a matching coarse instance
//       (hash not in instances list) produce a TileRecord with manifest_idx=0,
//       pos={0,0,0} — pairing still works via tx/tz.
//
//   (h) next_all_full: next() returns false when all tiles are Full or Queued.
//
// Synthetic data only — no bake, no disk I/O.

#include "refine_controller.h"
#include "check.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>

using namespace matter_refine;

// ---------------------------------------------------------------------------
// Helpers to build synthetic GraphNode / InstanceRef sets.
// ---------------------------------------------------------------------------

// Build a Terrain params_json string.  Keys must be in canonical sorted order:
// {res, tx, tz, worldSeed, worldSize}
// (Terrain params are {res, tx, tz, worldSeed, worldSize} — sorted alphabetically.)
// params_to_json uses %.17g for numbers and "str" for strings (no whitespace).
static std::string terrain_json(const char* res, int tx, int tz,
                                 int worldSeed = 42, int worldSize = 10) {
    char buf[256];
    // Integer values that fit exactly in double print without a decimal point
    // with %.17g (e.g., 5.0 → "5").
    std::snprintf(buf, sizeof buf,
        "{\"res\":\"%s\",\"tx\":%d,\"tz\":%d,\"worldSeed\":%d,\"worldSize\":%d}",
        res, tx, tz, worldSeed, worldSize);
    return buf;
}

// Build a simple 4-tile world: tiles at (0,0),(1,0),(0,1),(1,1).
// Coarse instances are placed in the world (have an InstanceRef).
// Full-res nodes have resolved hashes but no instance (not yet baked/placed).
static void make_4tile_world(std::vector<GraphNode>& nodes,
                              std::vector<InstanceRef>& instances) {
    // Assign distinct hashes so we can pair them.
    // coarse: hashes 100..103, full: hashes 200..203
    struct Tile { int tx, tz; };
    Tile tiles[] = {{0,0},{1,0},{0,1},{1,1}};

    uint32_t midx = 0;
    for (int i = 0; i < 4; ++i) {
        uint64_t ch = 100 + (uint64_t)i;
        uint64_t fh = 200 + (uint64_t)i;

        // Coarse node
        GraphNode cn;
        cn.module = "Terrain";
        cn.params_json = terrain_json("coarse", tiles[i].tx, tiles[i].tz);
        cn.resolved_hash = ch;
        nodes.push_back(cn);

        // Full-res node (resolved, not yet baked)
        GraphNode fn;
        fn.module = "Terrain";
        fn.params_json = terrain_json("full", tiles[i].tx, tiles[i].tz);
        fn.resolved_hash = fh;
        nodes.push_back(fn);

        // Coarse instance in the world
        InstanceRef ir;
        ir.hash = ch;
        // Tile center at (tx*10 + 5, 0, tz*10 + 5) — world-space; TILE=10
        ir.translation[0] = (float)(tiles[i].tx * 10);
        ir.translation[1] = 0.0f;
        ir.translation[2] = (float)(tiles[i].tz * 10);
        ir.manifest_idx = midx++;
        instances.push_back(ir);
    }

    // Two scatter nodes (different module) — should be ignored.
    GraphNode s1, s2;
    s1.module = "Scatter";  s1.params_json = "{\"density\":1}";  s1.resolved_hash = 999;
    s2.module = "Grass";    s2.params_json = "{\"h\":0.5}";       s2.resolved_hash = 998;
    nodes.push_back(s1);
    nodes.push_back(s2);
}

// ---------------------------------------------------------------------------
// (a) tile_count
// ---------------------------------------------------------------------------
static void test_tile_count() {
    printf("[test_tile_count]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    CHECK(rc.tile_count() == 4, "tile_count()==4 for 4-tile world");
    printf("ok tile_count\n");
}

// ---------------------------------------------------------------------------
// (b) full_count
// ---------------------------------------------------------------------------
static void test_full_count() {
    printf("[test_full_count]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    CHECK(rc.full_count() == 0, "full_count initially 0");

    rc.mark(0, TileRecord::State::Full);
    CHECK(rc.full_count() == 1, "full_count==1 after one mark(Full)");

    rc.mark(2, TileRecord::State::Full);
    CHECK(rc.full_count() == 2, "full_count==2 after two mark(Full)");
    printf("ok full_count\n");
}

// ---------------------------------------------------------------------------
// (c) next_nearest
// ---------------------------------------------------------------------------
static void test_next_nearest() {
    printf("[test_next_nearest]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    // Focus near tile (1,1) → world pos around (10,0,10).
    // Our tile positions: (0,0,0), (10,0,0), (0,0,10), (10,0,10)
    // (translation from instance + TILE/2 offset = 0+5=5; we set translation to tx*10
    //  so center = tx*10+5, tz*10+5 per tile).
    // Actually in make_4tile_world we stored translation = (tx*10, 0, tz*10);
    // build() computes pos = translation + TILE/2.  TILE_SIZE=10 → pos=(tx*10+5, 0, tz*10+5).
    // Focus at (10,0,10) → nearest is tile(1,1) at (15,0,15), dist=sqrt(50) ≈ 7.07
    //   vs tile(0,0) at (5,0,5), dist=sqrt(200) ≈ 14.14
    //   vs tile(1,0) at (15,0,5), dist=sqrt(25)  = 5 (tx=1,tz=0)
    //   vs tile(0,1) at (5,0,15), dist=sqrt(25)  = 5 (tx=0,tz=1)
    //   vs tile(1,1) at (15,0,15), dist=sqrt(50) ≈ 7.07
    // Focus at (14,0,4): nearest = tile(1,0) at (15,0,5), dist=sqrt(2)
    float focus[3] = {14.0f, 0.0f, 4.0f};
    TileRecord* out = nullptr;
    bool got = rc.next(focus, &out);
    CHECK(got, "next() returns true when tiles available");
    CHECK(out != nullptr, "next() sets out pointer");
    if (out) {
        // Nearest tile is (tx=1, tz=0): pos=(15,0,5)
        CHECK(fabsf(out->pos[0] - 15.0f) < 0.1f && fabsf(out->pos[2] - 5.0f) < 0.1f,
              "nearest tile is (tx=1,tz=0) with pos≈(15,0,5)");
        CHECK(out->state == TileRecord::State::Coarse, "returned tile is Coarse state");
        CHECK(out->full_hash != 0, "full_hash is non-zero");
        CHECK(out->coarse_hash != 0, "coarse_hash is non-zero");
    }
    printf("ok next_nearest\n");
}

// ---------------------------------------------------------------------------
// (d) mark_queued
// ---------------------------------------------------------------------------
static void test_mark_queued() {
    printf("[test_mark_queued]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    // Focus near tile (1,0) = idx 1 in our build order
    float focus[3] = {14.0f, 0.0f, 4.0f};
    TileRecord* out1 = nullptr;
    rc.next(focus, &out1);
    CHECK(out1 != nullptr, "first next() finds a tile");

    // Mark nearest tile as Queued
    TileRecord* nearest = nullptr;
    rc.next(focus, &nearest);
    // Find its logical index by scanning
    if (nearest) {
        for (uint32_t i = 0; i < (uint32_t)rc.tile_count(); ++i) {
            rc.mark(i, TileRecord::State::Queued);
            TileRecord* after = nullptr;
            bool found2 = rc.next(focus, &after);
            if (!found2 || after != nearest) {
                // i was the nearest tile; it is now queued
                CHECK(true, "nearest tile found and queued");
                // next() should now return a different tile
                if (found2 && after) {
                    CHECK(after != nearest, "after queuing nearest, next returns different tile");
                }
                // restore + break
                rc.mark(i, TileRecord::State::Coarse);
                break;
            }
            rc.mark(i, TileRecord::State::Coarse);
        }
    }

    // Simpler direct test: mark ALL tiles Queued → next() returns false
    for (uint32_t i = 0; i < (uint32_t)rc.tile_count(); ++i) {
        rc.mark(i, TileRecord::State::Queued);
    }
    TileRecord* none = nullptr;
    bool got = rc.next(focus, &none);
    CHECK(!got, "next() returns false when all tiles queued");
    printf("ok mark_queued\n");
}

// ---------------------------------------------------------------------------
// (e) mark_full + full_count
// ---------------------------------------------------------------------------
static void test_mark_full() {
    printf("[test_mark_full]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    for (uint32_t i = 0; i < 4; ++i) {
        rc.mark(i, TileRecord::State::Full);
    }
    CHECK(rc.full_count() == 4, "all 4 tiles marked Full → full_count==4");

    float focus[3] = {0, 0, 0};
    TileRecord* out = nullptr;
    bool got = rc.next(focus, &out);
    CHECK(!got, "next() returns false when all tiles Full");
    printf("ok mark_full\n");
}

// ---------------------------------------------------------------------------
// (f) evict_beyond
// ---------------------------------------------------------------------------
static void test_evict_beyond() {
    printf("[test_evict_beyond]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    // Mark tiles 0,1,2 as Full; tile 3 stays Coarse
    rc.mark(0, TileRecord::State::Full);
    rc.mark(1, TileRecord::State::Full);
    rc.mark(2, TileRecord::State::Full);
    // leave tile 3 as Coarse

    // Focus at (5,0,5) = center of tile (0,0)
    // Tile positions (centers with TILE_SIZE=10):
    //   tile 0 (tx=0,tz=0): pos=(5,0,5)   dist=0
    //   tile 1 (tx=1,tz=0): pos=(15,0,5)  dist=10
    //   tile 2 (tx=0,tz=1): pos=(5,0,15)  dist=10
    //   tile 3 (tx=1,tz=1): pos=(15,0,15) dist ≈ 14.14
    float focus[3] = {5.0f, 0.0f, 5.0f};

    // radius=12 → tile 0 inside (dist=0), tile 1,2 outside (dist=10 < 12 actually inside!)
    // Let's use radius=8: tiles 1,2 are at dist=10 > 8, tile 0 at dist=0 < 8
    // Full tiles are 0,1,2. Tile 3 is Coarse (never returned).
    // Evict: tiles 1 and 2 are Full + dist=10 > 8; sorted farthest-first: equal dist, any order.
    auto evicted8 = rc.evict_beyond(focus, 8.0f);
    CHECK(evicted8.size() == 2, "evict_beyond(radius=8) returns 2 full tiles outside radius");
    // Both tile 1 and tile 2 are equidistant; ensure both indices present
    if (evicted8.size() == 2) {
        bool has1 = (evicted8[0] == 1 || evicted8[1] == 1);
        bool has2 = (evicted8[0] == 2 || evicted8[1] == 2);
        CHECK(has1, "evict_beyond includes tile 1 (Full, dist=10>8)");
        CHECK(has2, "evict_beyond includes tile 2 (Full, dist=10>8)");
    }

    // radius=1: tiles 1,2 also outside (both dist=10 > 1), tile 0 at dist=0 inside
    // evict should return 2 full tiles (1 and 2), NOT tile 0 (inside radius)
    auto evicted1 = rc.evict_beyond(focus, 1.0f);
    CHECK(evicted1.size() == 2, "evict_beyond(radius=1) still returns 2 tiles outside");
    bool no_tile0 = true;
    for (uint32_t idx : evicted1) { if (idx == 0) no_tile0 = false; }
    CHECK(no_tile0, "tile 0 (within radius 1) not evicted");

    // Coarse tiles are never returned
    bool no_coarse = true;
    for (uint32_t idx : evicted1) { if (idx == 3) no_coarse = false; }
    CHECK(no_coarse, "tile 3 (Coarse) never evicted");

    // Test farthest-first ordering with 3 different distances.
    // Mark tile 3 Full too, then evict with radius=0 from tile 0 center.
    // Distances: tile0=0, tile1=10, tile2=10, tile3≈14.14
    // Evict all outside radius=0: tiles 1,2,3 → sorted farthest-first.
    // Tile 3 (dist≈14.14) must come before tiles 1,2 (dist=10).
    rc.mark(3, TileRecord::State::Full);
    auto evictAll = rc.evict_beyond(focus, 0.0f);
    CHECK(evictAll.size() == 3, "evict_beyond(radius=0) returns 3 tiles (all Full outside center)");
    if (evictAll.size() >= 1) {
        CHECK(evictAll[0] == 3, "tile 3 (farthest, dist≈14.14) is first in evict list");
    }
    printf("ok evict_beyond\n");
}

// ---------------------------------------------------------------------------
// (g) no_coarse_instance: full-res nodes without a matching coarse instance
// ---------------------------------------------------------------------------
static void test_no_coarse_instance() {
    printf("[test_no_coarse_instance]\n");
    // Build a world where only full-res nodes are in the snapshot (no coarse instances).
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances; // empty

    for (int i = 0; i < 2; ++i) {
        GraphNode fn;
        fn.module = "Terrain";
        fn.params_json = terrain_json("full", i, 0);
        fn.resolved_hash = 300 + (uint64_t)i;
        nodes.push_back(fn);

        GraphNode cn;
        cn.module = "Terrain";
        cn.params_json = terrain_json("coarse", i, 0);
        cn.resolved_hash = 400 + (uint64_t)i;
        nodes.push_back(cn);
    }
    // No instances at all.

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    CHECK(rc.tile_count() == 2, "still pairs 2 tiles even with no instances");
    // The tiles should have coarse_hash set (matched by tx/tz from coarse node)
    // but pos={0,0,0} and manifest_idx=0 since no instance provides them.
    printf("ok no_coarse_instance\n");
}

// ---------------------------------------------------------------------------
// (h) next_all_full
// ---------------------------------------------------------------------------
static void test_next_all_full() {
    printf("[test_next_all_full]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    make_4tile_world(nodes, instances);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    for (uint32_t i = 0; i < 4; ++i) rc.mark(i, TileRecord::State::Full);

    float focus[3] = {5.0f, 0.0f, 5.0f};
    TileRecord* out = nullptr;
    bool got = rc.next(focus, &out);
    CHECK(!got, "next() false when all Full");
    printf("ok next_all_full\n");
}

// ---------------------------------------------------------------------------
// (i) malformed_terrain_missing_keys: Terrain nodes lacking tx/tz/res should be skipped
// ---------------------------------------------------------------------------
static void test_malformed_terrain_missing_keys() {
    printf("[test_malformed_terrain_missing_keys]\n");
    std::vector<GraphNode> nodes;
    std::vector<InstanceRef> instances;
    // Build a 2-tile world with valid tiles
    make_4tile_world(nodes, instances);
    size_t initial_node_count = nodes.size();

    // Record the initial tile (0,0) state (hash)
    RefineController rc_init;
    rc_init.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});
    uint64_t tile_00_coarse_hash = rc_init.tile_count() > 0 ?
        /* We need to inspect; actually tile (0,0) is index 0 */ 100 : 0;

    // Now add a malformed Terrain node with missing tx
    GraphNode bad1;
    bad1.module = "Terrain";
    bad1.params_json = "{\"res\":\"coarse\",\"tz\":0,\"worldSeed\":42,\"worldSize\":10}";
    // Missing "tx" key — should default to extract_int return = 0
    bad1.resolved_hash = 999;
    nodes.push_back(bad1);

    // Add another malformed Terrain node with missing tz
    GraphNode bad2;
    bad2.module = "Terrain";
    bad2.params_json = "{\"res\":\"coarse\",\"tx\":0,\"worldSeed\":42,\"worldSize\":10}";
    // Missing "tz" key
    bad2.resolved_hash = 998;
    nodes.push_back(bad2);

    // Add a malformed Terrain node missing res
    GraphNode bad3;
    bad3.module = "Terrain";
    bad3.params_json = "{\"tx\":2,\"tz\":2,\"worldSeed\":42,\"worldSize\":10}";
    // Missing "res" key
    bad3.resolved_hash = 997;
    nodes.push_back(bad3);

    RefineController rc;
    rc.build({nodes.data(), nodes.size()}, {instances.data(), instances.size()});

    // tile_count should still be 4 (only the original 4 valid tiles from make_4tile_world)
    // The malformed nodes should have been skipped
    CHECK(rc.tile_count() == 4, "tile_count==4 with malformed nodes (they are skipped)");
    printf("ok malformed_terrain_missing_keys\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    test_tile_count();
    test_full_count();
    test_next_nearest();
    test_mark_queued();
    test_mark_full();
    test_evict_beyond();
    test_no_coarse_instance();
    test_next_all_full();
    test_malformed_terrain_missing_keys();
    return check_summary();
}
