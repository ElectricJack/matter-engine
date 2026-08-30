#include "../include/mesh_charting.h"
#include <cstdio>
#include <cmath>
using namespace mesh_charting;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

// Two triangles sharing an edge form a quad; they must be mutual neighbors.
static void test_adjacency_quad() {
    float pos[] = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    unsigned short idx[] = {0,1,2, 0,2,3}; // shared edge (0,2)
    auto adj = build_adjacency(pos, idx, 2);
    bool linked = (adj[0].nbr[0]==1||adj[0].nbr[1]==1||adj[0].nbr[2]==1) &&
                  (adj[1].nbr[0]==0||adj[1].nbr[1]==0||adj[1].nbr[2]==0);
    CHECK(linked, "quad triangles are mutual neighbors");
}

// A coplanar quad is one chart at a 30-degree cone.
static void test_segment_one_chart() {
    float pos[] = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    unsigned short idx[] = {0,1,2, 0,2,3};
    auto adj = build_adjacency(pos, idx, 2);
    int n=0; auto cid = segment_charts(pos, idx, 2, adj, 30.0f, n);
    CHECK(n==1, "coplanar quad is a single chart");
    CHECK(cid[0]==cid[1], "both tris in same chart");
}

static void test_plane_basis_orthonormal() {
    float n[3]={0,0,1}, T[3],B[3]; plane_basis(n,T,B);
    float dotTB=T[0]*B[0]+T[1]*B[1]+T[2]*B[2];
    float dotTN=T[0]*n[0]+T[1]*n[1]+T[2]*n[2];
    CHECK(std::fabs(dotTB)<1e-5f, "T perp B");
    CHECK(std::fabs(dotTN)<1e-5f, "T perp N");
}

static void test_pack_fits() {
    std::vector<ChartRect> r = {{0,0,1,1},{0,0,1,1}};
    float scale=1; std::vector<ChartPlacement> pl;
    CHECK(pack_charts(r, 64, 64, 2, scale, pl), "two unit charts pack into 64x64");
    CHECK(pl.size()==2, "placement per chart");
}

// Non-manifold input must still produce a SYMMETRIC graph. Three triangles
// share the edge (0,2); the first two to claim it pair up and the third gets
// no neighbour across it. The old code re-linked the third against the stale
// first claimant, leaving tri 0 pointing at tri 2 while tri 1 still pointed at
// tri 0 — a one-way edge that segment_charts flood-fills through in one
// direction only.
static void test_adjacency_non_manifold_stays_symmetric() {
    // Four vertices around the shared edge (v0,v2), three fans off it.
    float pos[] = {0,0,0,  1,0,0,  1,1,0,  0,1,0,  1,0,1};
    unsigned int idx[] = {0,1,2,  0,2,3,  0,2,4};
    auto adj = build_adjacency(pos, idx, 3);

    int oneway = 0, links = 0;
    for (int t = 0; t < 3; ++t) {
        for (int e = 0; e < 3; ++e) {
            const int nb = adj[t].nbr[e];
            if (nb < 0) continue;
            ++links;
            bool back = false;
            for (int f = 0; f < 3; ++f) if (adj[nb].nbr[f] == t) back = true;
            if (!back) ++oneway;
        }
    }
    CHECK(oneway == 0, "non-manifold adjacency has no one-way links");
    CHECK(links == 2, "exactly one pair claims the shared edge (2 half-links)");
}

// Every failure path must leave the outputs empty rather than handing back the
// last rejected attempt's coordinates.
static void test_pack_failure_clears_outputs() {
    // Prime both outputs with junk so a failure that forgets to clear shows up.
    float scale = 7.5f;
    std::vector<ChartPlacement> pl = {{11,22},{33,44}};

    std::vector<ChartRect> empty;
    CHECK(!pack_charts(empty, 64, 64, 2, scale, pl), "an empty chart list fails");
    CHECK(pl.empty() && scale == 0.0f, "empty-input failure clears both outputs");

    scale = 7.5f; pl = {{11,22}};
    std::vector<ChartRect> one = {{0,0,1,1}};
    CHECK(!pack_charts(one, 0, 64, 2, scale, pl), "a zero atlas dimension fails");
    CHECK(pl.empty() && scale == 0.0f, "bad-dimension failure clears both outputs");

    // Unpackable: a 4-texel atlas with an 8-texel gutter on every side.
    scale = 7.5f; pl = {{11,22}};
    CHECK(!pack_charts(one, 4, 4, 8, scale, pl), "a chart larger than the atlas fails");
    CHECK(pl.empty() && scale == 0.0f, "exhausted-attempts failure clears both outputs");
}

// The paged packer's success and failure contracts.
static void test_pack_paged() {
    std::vector<PagedChartSize> charts = {{100,100},{60,200},{10,10}};
    int aw = -1, ah = -1;
    std::vector<PagedChartPlacement> pl;
    CHECK(pack_charts_paged(charts, 64, 4, 4096, aw, ah, pl), "three charts pack");
    CHECK(pl.size() == 3, "a placement per chart");
    bool aligned = aw % 64 == 0 && ah % 64 == 0;
    for (const auto& p : pl)
        aligned = aligned && p.x % 64 == 0 && p.y % 64 == 0 &&
                             p.w % 64 == 0 && p.h % 64 == 0;
    CHECK(aligned, "atlas and every block are page multiples");

    // A single block wider than the atlas: rejected, outputs cleared.
    std::vector<PagedChartSize> huge = {{5000,10}};
    aw = -1; ah = -1; pl = {{1,2,3,4}};
    CHECK(!pack_charts_paged(huge, 64, 4, 256, aw, ah, pl), "an oversized chart fails");
    CHECK(pl.empty() && aw == 0 && ah == 0, "paged failure clears its outputs");

    // Empty input is a failure, not a zero-size success.
    std::vector<PagedChartSize> none;
    aw = -1; ah = -1; pl = {{1,2,3,4}};
    CHECK(!pack_charts_paged(none, 64, 4, 256, aw, ah, pl), "an empty chart list fails");
    CHECK(pl.empty() && aw == 0 && ah == 0, "empty-input paged failure clears outputs");
}

int main(){
    test_adjacency_quad(); test_segment_one_chart();
    test_adjacency_non_manifold_stays_symmetric();
    test_plane_basis_orthonormal(); test_pack_fits();
    test_pack_failure_clears_outputs();
    test_pack_paged();
    if(!failures) printf("All mesh_charting tests passed\n");
    return failures?1:0;
}
