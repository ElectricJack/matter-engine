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

int main(){
    test_adjacency_quad(); test_segment_one_chart();
    test_plane_basis_orthonormal(); test_pack_fits();
    if(!failures) printf("All mesh_charting tests passed\n");
    return failures?1:0;
}
