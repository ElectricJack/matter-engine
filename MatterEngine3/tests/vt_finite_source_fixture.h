#pragma once
#include "render/vt_finite_sources.h"
#include <fstream>
namespace vt_finite_test {
// Same-build, native little-endian evidence interchange between the projection
// and VT integration tests. This is deliberately not a production asset format.
inline bool save(const std::string &path,const surface_stamp::Stamp &s) {
    if (s.height_projection!=1) return false;
    std::ofstream out(path,std::ios::binary);
    const uint64_t keys[]={0x3154504d41545346ull,1,s.geometry_digest,s.material_digest,s.content_digest,s.pixels.size()};
    const uint32_t directory[]={uint32_t(s.levels.size()),1};
    const auto &f=s.frame;
    const float metadata[]={f.origin_m.x,f.origin_m.y,f.origin_m.z,f.u.x,f.u.y,f.u.z,
        f.v.x,f.v.y,f.v.z,f.n.x,f.n.y,f.n.z,s.domain[0],s.domain[1],s.domain[2],s.domain[3],
        s.height_min_m,s.height_max_m,s.projection_error_m,0};
    out.write(reinterpret_cast<const char*>(keys),sizeof(keys));
    out.write(reinterpret_cast<const char*>(directory),sizeof(directory));
    out.write(reinterpret_cast<const char*>(metadata),sizeof(metadata));
    out.write(reinterpret_cast<const char*>(s.levels.data()),s.levels.size()*sizeof(s.levels[0]));
    out.write(reinterpret_cast<const char*>(s.pixels.data()),s.pixels.size()*sizeof(s.pixels[0]));
    out.close();return bool(out);
}
inline std::shared_ptr<const surface_stamp::Stamp> load(const std::string &path) {
    std::ifstream in(path,std::ios::binary);
    uint64_t keys[6]{};uint32_t directory[2]{};float m[20]{};
    in.read(reinterpret_cast<char*>(keys),sizeof(keys));
    in.read(reinterpret_cast<char*>(directory),sizeof(directory));
    in.read(reinterpret_cast<char*>(m),sizeof(m));
    if (!in || keys[0]!=0x3154504d41545346ull || keys[1]!=1 || !keys[5] ||
        keys[5]>128u*1024u*1024u/sizeof(surface_stamp::Channels) ||
        !directory[0] || directory[0]>32 || directory[1]!=1 || m[19]!=0) return {};
    auto s=std::make_shared<surface_stamp::Stamp>();
    s->geometry_digest=keys[2];s->material_digest=keys[3];s->content_digest=keys[4];
    s->frame={{m[0],m[1],m[2]},{m[3],m[4],m[5]},{m[6],m[7],m[8]},{m[9],m[10],m[11]}};
    std::copy(m+12,m+16,s->domain);s->height_min_m=m[16];s->height_max_m=m[17];
    s->projection_error_m=m[18];s->height_projection=1;
    s->levels.resize(directory[0]);s->pixels.resize(size_t(keys[5]));
    in.read(reinterpret_cast<char*>(s->levels.data()),s->levels.size()*sizeof(s->levels[0]));
    in.read(reinterpret_cast<char*>(s->pixels.data()),s->pixels.size()*sizeof(s->pixels[0]));
    if (!in || in.peek()!=std::char_traits<char>::eof()) return {};
    return s;
}
inline std::shared_ptr<const surface_stamp::Stamp> source(bool edited=false,uint32_t edge=2) {
    auto s=std::make_shared<surface_stamp::Stamp>();
    s->height_projection=1;s->height_min_m=s->height_max_m=edited?.04f:.02f;
    s->domain[0]=s->domain[1]=-.5f;s->domain[2]=s->domain[3]=1;
    s->geometry_digest=91;s->material_digest=edited?93:92;s->content_digest=(edited?95:94)+uint64_t(edge)*256;
    for (uint32_t size=edge;;size=std::max(1u,size/2)) {
        s->levels.push_back({uint32_t(s->pixels.size()),size,size,0});
        s->pixels.resize(s->pixels.size()+size_t(size)*size);
        if (size==1) break;
    }
    for (auto &p:s->pixels) {
        p.albedo_coverage[0]=edited?.1f:.8f;p.albedo_coverage[1]=.15f;
        p.albedo_coverage[2]=edited?.8f:.08f;p.albedo_coverage[3]=1;
        p.orm_height[0]=.9f;p.orm_height[1]=.09f;p.orm_height[3]=s->height_min_m;
        p.normal_detail[2]=p.geometric_reserved[2]=1;
    }
    return s;
}
inline vt::VtFiniteSourceBinding binding(std::shared_ptr<const surface_stamp::Stamp> s) {
    vt::VtFiniteSourceBinding b;b.stamp=std::move(s);
    b.frame.origin_m={.9375f,0,.9375f};b.frame.u={1,0,0};b.frame.v={0,0,-1};b.frame.n={0,1,0};
    return b;
}
inline const char *base() {
    return "const 0.2\nconst 0.7\nconst 0\nconst 1\nconst -0.03\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.03 -0.03\n";
}
}
