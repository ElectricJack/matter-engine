#pragma once
#include <matter/geometry/geometry.h>
#include <map>
#include <utility>
namespace geometry_fixture {
namespace d=matter::contracts;
inline std::vector<d::Vec3> sphere_corners(int levels) {
    std::vector<d::Vec3> v{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    std::vector<uint32_t> indices{0,2,4, 2,1,4, 1,3,4, 3,0,4, 2,0,5, 1,2,5, 3,1,5, 0,3,5};
    for(int l=0;l<levels;++l) {
        std::map<std::pair<uint32_t,uint32_t>,uint32_t> mid;
        auto midpoint=[&](uint32_t a,uint32_t b) {
            auto key=a<b?std::make_pair(a,b):std::make_pair(b,a);
            auto it=mid.find(key);
            if(it!=mid.end()) return it->second;
            v.push_back(mm::normalize(mm::scale_vec(mm::add(v[a],v[b]),0.5f)));
            return mid[key]=uint32_t(v.size()-1);
        };
        std::vector<uint32_t> out;
        for(size_t t=0;t+2<indices.size();t+=3) {
            uint32_t a=indices[t],b=indices[t+1],c=indices[t+2],ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
            out.insert(out.end(),{a,ab,ca, b,bc,ab, c,ca,bc, ab,bc,ca});
        }
        indices=out;
    }
    std::vector<d::Vec3> corners;
    for(auto i:indices) corners.push_back(v[i]);
    return corners;
}
}
