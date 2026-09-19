#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vt {
// Count physical placements, including hidden/out-of-view nodes. A variant's
// pages can carry one world frame only until per-instance overlays exist.
struct VtWorldReceiverFrame {
    uint32_t references=0;
    std::array<float,16> local_to_world{};
    void add(const float* parent,const float* relative=nullptr) {
        ++references;
        if(references!=1) return;
        if(!relative) {std::memcpy(local_to_world.data(),parent,16*sizeof(float));return;}
        for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
            for(unsigned k=0;k<4;++k) local_to_world[r*4+c]+=parent[r*4+k]*relative[k*4+c];
    }
    bool supported() const {
        if(references!=1) return false;
        const auto& m=local_to_world;
        for(float x:m) if(!std::isfinite(x)) return false;
        if(m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1) return false;
        for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) {
            float d=0;for(unsigned k=0;k<3;++k)d+=m[r*4+k]*m[c*4+k];
            if(std::abs(d-(r==c?1.f:0.f))>1e-5f)return false;
        }
        const float determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-
            m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);
        return determinant>0.9999f;
    }
};
} // namespace vt
