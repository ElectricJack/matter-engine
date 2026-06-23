#include "../include/voxel_imposter.h"
#include <cstdio>
#include <cmath>
using namespace voxel_imposter;
static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_grid_dims_cube() {
    float lo[3]={0,0,0}, hi[3]={2,2,2}; int nx,ny,nz;
    CHECK(choose_grid_dims(lo,hi,128,nx,ny,nz), "cube dims ok");
    CHECK(nx==128 && ny==128 && nz==128, "cube -> 128^3");
}
static void test_grid_dims_flat() {
    float lo[3]={0,0,0}, hi[3]={4,4,1}; int nx,ny,nz; // z is 1/4 the extent
    CHECK(choose_grid_dims(lo,hi,128,nx,ny,nz), "flat dims ok");
    CHECK(nx==128 && ny==128 && nz==32, "flat brick -> 128x128x32");
}
static void test_grid_dims_degenerate() {
    float lo[3]={0,0,0}, hi[3]={0,0,0}; int nx,ny,nz;
    CHECK(!choose_grid_dims(lo,hi,128,nx,ny,nz), "zero extent rejected");
}

int main(){
    test_grid_dims_cube(); test_grid_dims_flat(); test_grid_dims_degenerate();
    if(!failures) printf("All voxel_imposter tests passed\n");
    return failures?1:0;
}
