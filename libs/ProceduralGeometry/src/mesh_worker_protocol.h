#pragma once
// Private, versioned local-process wire format. No pointers, padding or native
// vector layouts cross the boundary. Integers and float bits are little-endian.
#include "mesh_indexed.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace matter::geometry::wire {
static_assert(sizeof(float)==4, "mesh worker requires float32");
constexpr uint32_t magic = 0x31474d4d; // MMG1
constexpr int resource_exit = 6;
constexpr uint64_t hard_bytes = 256ull * 1024 * 1024;
struct Options {
    float target_ratio = 1;
    uint32_t iterations = 3, seed = 0, timeout_seconds = 60;
};
struct Limits {
    uint64_t vertices, indices, bytes;
};
inline void require(bool ok) { if (!ok) throw std::runtime_error("invalid mesh worker payload"); }
inline void put(std::ostream &s, uint32_t n) {
    for (int k=0;k<4;++k) s.put(char((n>>(k*8))&255));
    require(bool(s));
}
inline uint32_t get(std::istream &s) {
    uint32_t n=0;
    for (int k=0;k<4;++k) { int b=s.get(); require(b!=EOF); n|=uint32_t(b)<<(k*8); }
    return n;
}
inline void put_float(std::ostream &s,float v) {
    require(std::isfinite(v)); uint32_t n; std::memcpy(&n,&v,4); put(s,n);
}
inline float get_float(std::istream &s) {
    auto n=get(s); float v; std::memcpy(&v,&n,4); require(std::isfinite(v)); return v;
}
inline void put3(std::ostream &s,float3 p) { put_float(s,p.x); put_float(s,p.y); put_float(s,p.z); }
inline float3 get3(std::istream &s) {
    float x=get_float(s),y=get_float(s),z=get_float(s); return make_float3(x,y,z);
}
inline void put2(std::ostream &s,float2 p) { put_float(s,p.x); put_float(s,p.y); }
inline float2 get2(std::istream &s) {
    float x=get_float(s),y=get_float(s); return make_float2(x,y);
}
inline uint64_t size(uint64_t v,uint64_t i) { return 12+v*12+i*4+(i/3)*92; }
inline void counts(uint64_t v,uint64_t i,Limits l) {
    require(v && i && i%3==0 && v<=UINT32_MAX && i<=UINT32_MAX);
    require(v<=l.vertices && i<=l.indices && size(v,i)<=l.bytes && size(v,i)<=hard_bytes);
}
inline void write(std::ostream &s,const MeshIndexed &m,Limits l) {
    counts(m.positions.size(),m.indices.size(),l);
    require(m.triex.size()==m.indices.size()/3);
    put(s,magic); put(s,uint32_t(m.positions.size())); put(s,uint32_t(m.indices.size()));
    for(auto p:m.positions) put3(s,p);
    for(auto i:m.indices) { require(i<m.positions.size()); put(s,i); }
    for(const auto &e:m.triex) {
        put2(s,e.uv0); put2(s,e.uv1); put2(s,e.uv2);
        put3(s,e.N0); put3(s,e.N1); put3(s,e.N2);
        put_float(s,e.tint.x); put_float(s,e.tint.y); put_float(s,e.tint.z); put_float(s,e.tint.w);
        put_float(s,e.ao0); put_float(s,e.ao1); put_float(s,e.ao2); put(s,uint32_t(e.materialId));
    }
}
inline MeshIndexed read(std::istream &s,Limits l) {
    require(get(s)==magic); uint32_t v=get(s),i=get(s); counts(v,i,l);
    MeshIndexed m; m.positions.resize(v); m.indices.resize(i); m.triex.resize(i/3);
    for(auto &p:m.positions) p=get3(s);
    for(auto &index:m.indices) { index=get(s); require(index<v); }
    for(auto &e:m.triex) {
        e.uv0=get2(s); e.uv1=get2(s); e.uv2=get2(s);
        e.N0=get3(s); e.N1=get3(s); e.N2=get3(s);
        float x=get_float(s),y=get_float(s),z=get_float(s),w=get_float(s); e.tint=make_float4(x,y,z,w);
        e.ao0=get_float(s); e.ao1=get_float(s); e.ao2=get_float(s);
        auto material=get(s); require(material<=INT32_MAX); e.materialId=int(material);
    }
    return m;
}
inline void write_options(std::ostream &s,const Options &o,Limits l) {
    put(s,magic); put_float(s,o.target_ratio); put(s,o.iterations); put(s,o.seed); put(s,o.timeout_seconds);
    put(s,uint32_t(std::min(l.vertices,uint64_t(UINT32_MAX))));
    put(s,uint32_t(std::min(l.indices,uint64_t(UINT32_MAX)))); put(s,uint32_t(std::min(l.bytes,hard_bytes)));
}
inline Options read_options(std::istream &s,Limits &l) {
    require(get(s)==magic); Options o;
    o.target_ratio=get_float(s); o.iterations=get(s); o.seed=get(s); o.timeout_seconds=get(s);
    l.vertices=get(s); l.indices=get(s); l.bytes=get(s);
    require(o.target_ratio>0 && o.target_ratio<=4 && o.iterations>=1 && o.iterations<=1024 && o.timeout_seconds<=3600);
    require(l.bytes<=hard_bytes); return o;
}
} // namespace matter::geometry::wire
