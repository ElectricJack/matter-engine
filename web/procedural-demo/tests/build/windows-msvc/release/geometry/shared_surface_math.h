#pragma once
#include <cmath>
#include <matter_math_c.h>
namespace matter::geometry::detail {
using std::isinf; using std::isfinite;
[[maybe_unused]] static float smin_set(const float* vals, int n, float fmin, float k) {
    if (n <= 0) return INFINITY;
    if (k <= 1e-5f || n == 1) return fmin;
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += expf(-(vals[i] - fmin) / k);
    return fmin - k * logf(sum);
}


[[maybe_unused]] static MtVec3 VertexInterpolation(MtVec3 v1, float val1, MtVec3 v2, float val2, float isovalue) {
    MtVec3 result;

    // An empty spatial-hash query deliberately uses +INFINITY as the outside
    // field value. A coarse cell may straddle that sentinel and a finite inside
    // sample. Handle both edge orientations explicitly: the generic quotient
    // is otherwise inf/inf in one direction and emits a NaN vertex. Snapping to
    // the finite endpoint also preserves the result the opposite orientation
    // already produced under IEEE arithmetic.
    if (isinf(val1) && val1 > 0.0f && isfinite(val2)) return v2;
    if (isfinite(val1) && isinf(val2) && val2 > 0.0f) return v1;
    
    if (fabs(isovalue - val1) < 0.00001f) return v1;
    if (fabs(isovalue - val2) < 0.00001f) return v2;
    if (fabs(val1 - val2) < 0.00001f) return v1;
    
    float mu = (isovalue - val1) / (val2 - val1);
    
    result.x = v1.x + mu * (v2.x - v1.x);
    result.y = v1.y + mu * (v2.y - v1.y);
    result.z = v1.z + mu * (v2.z - v1.z);
    
    return result;
}



}
