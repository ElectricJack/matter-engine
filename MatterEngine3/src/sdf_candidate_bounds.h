#pragma once

#include "fat_primitive.h"
#include "matter_math.h"
#include <algorithm>
#include <cmath>

namespace sdf_candidates {

// A staged smooth union contains at most 128+64 distances. Its value obeys
// smin(d) >= min(d)-k*log(192). Thus field<=0 requires at least one primitive
// with d<=k*log(192). Ordered differences/intersections and trailing carve/clip
// only increase the field; the extractor's isovalue is <=0. This support bound
// therefore safely removes empty candidate cells without changing the lattice.
//
// For a box, d<=padding is contained in local halfExtents+padding. Transform
// that box back to world space. Keep other shapes and ill-conditioned matrices
// on their existing candidate bounds until independently validated.
inline bool box_support(const FatPrim& prim, float max_blend_width,
                        mm::Vec3& lower, mm::Vec3& upper) {
    if (prim.kind != FAT_PRIM_BOX || !std::isfinite(max_blend_width) || max_blend_width < 0)
        return false;
    const mm::Mat4 inv = mm::from_c(prim.invTransform);
    if (inv.m[12]!=0 || inv.m[13]!=0 || inv.m[14]!=0 || inv.m[15]!=1) return false;
    mm::Mat4 forward;
    if (!mm::inverse(inv, forward)) return false;
    float norm_inv=0, norm_forward=0;
    for (int r=0;r<3;++r) {
        float a=0,b=0;
        for(int c=0;c<3;++c) { a+=std::fabs(inv.m[r*4+c]); b+=std::fabs(forward.m[r*4+c]); }
        norm_inv=std::max(norm_inv,a); norm_forward=std::max(norm_forward,b);
    }
    if (!std::isfinite(norm_inv*norm_forward) || norm_inv*norm_forward>1000) return false;
    const float padding=max_blend_width*std::log(192.0f);
    const float h[]={prim.halfExtents.x,prim.halfExtents.y,prim.halfExtents.z};
    float lo[3],hi[3];
    for(int r=0;r<3;++r) {
        double extent=0;
        for(int c=0;c<3;++c) {
            if (!std::isfinite(h[c]) || h[c]<0) return false;
            extent+=std::fabs(double(forward.m[r*4+c]))*(double(h[c])+padding);
        }
        const double center=forward.m[r*4+3];
        // Outward slack covers float transform/SDF roundoff for the bounded
        // condition number above, including cancellation at translated brushes.
        const double slack=1e-4*(1+std::fabs(center)+extent)*std::max(1.0f,norm_inv*norm_forward);
        lo[r]=float(center-extent-slack); hi[r]=float(center+extent+slack);
        if(!std::isfinite(lo[r]) || !std::isfinite(hi[r])) return false;
    }
    lower={lo[0],lo[1],lo[2]}; upper={hi[0],hi[1],hi[2]};
    return true;
}
} // namespace sdf_candidates
