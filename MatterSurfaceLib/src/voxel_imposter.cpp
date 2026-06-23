#include "../include/voxel_imposter.h"
#include "../include/part_asset.h"   // fnv1a64 (used in later tasks)
#include <algorithm>
#include <cmath>
#include <cstring>

namespace voxel_imposter {

bool choose_grid_dims(const float lo[3], const float hi[3],
                      int maxDim, int& nx, int& ny, int& nz) {
    float ex = hi[0]-lo[0], ey = hi[1]-lo[1], ez = hi[2]-lo[2];
    float maxExtent = std::max(ex, std::max(ey, ez));
    if (maxExtent <= 0.0f || maxDim < 1) return false;
    float v = maxExtent / (float)maxDim;
    auto dim = [&](float e){ int d = (int)std::ceil(e / v); return std::max(1, std::min(maxDim, d)); };
    nx = dim(ex); ny = dim(ey); nz = dim(ez);
    return true;
}

} // namespace voxel_imposter
