#pragma once
#include "finite_surface_stamp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vt {
// One finite source bound to a planar receiver face. Coordinates and frame
// axes are physical part-local metres/unit vectors, with no implicit scaling.
struct VtFiniteSourceBinding {
    std::shared_ptr<const surface_stamp::Stamp> stamp;
    gpu_meshing::FaceFrame frame;
    float datum_m=0;
};
struct VtFiniteReceiver {
    gpu_meshing::FaceFrame frame;
    float datum_m=0;
    std::array<float,4> domain{};
    std::vector<uint32_t> sources; // zero-based catalog bindings
};
struct VtGpuFiniteSource {
    float origin_datum[4]{}, u[4]{}, v[4]{}, n[4]{}, domain[4]{};
    uint32_t levels[4]{}; // first level, count, reserved, reserved
};
static_assert(sizeof(VtGpuFiniteSource)==96,"finite source std430 ABI");
// Produced once, then shared by snapshots/rungs/instances. Pixels stay in their
// immutable sources on CPU; only the tiny directory is flattened. Identical
// source payloads have one upload range even if multiple face bindings use them.
struct VtFiniteSources {
    std::vector<std::shared_ptr<const surface_stamp::Stamp>> payloads;
    std::vector<surface_stamp::Level> levels;
    std::vector<VtGpuFiniteSource> bindings;
    std::vector<VtGpuFiniteSource> receivers;
    // Six std430 words per receiver followed by grid cells and candidate words.
    std::vector<std::array<uint32_t,4>> lookup;
    size_t pixel_count=0;
    float height_min_m=0,height_max_m=0;
    uint64_t content_hash=0, payload_hash=0;
    size_t bytes() const {
        size_t size=sizeof(*this)+payloads.capacity()*sizeof(payloads[0])+
            levels.capacity()*sizeof(levels[0])+bindings.capacity()*sizeof(bindings[0])+
            receivers.capacity()*sizeof(receivers[0])+lookup.capacity()*sizeof(lookup[0]);
        for (const auto &s:payloads) size+=sizeof(*s)+s->pixels.capacity()*sizeof(s->pixels[0])+s->levels.capacity()*sizeof(s->levels[0]);
        return size;
    }
};
inline uint64_t vt_finite_hash_word(uint64_t h,uint64_t word) {
    for (unsigned b=0;b<8;++b) { h^=uint8_t(word>>(b*8));h*=1099511628211ull; }
    return h;
}
inline bool vt_make_finite_sources(const std::vector<VtFiniteSourceBinding> &inputs,
    std::shared_ptr<const VtFiniteSources> &out,std::string &error,
    const std::vector<VtFiniteReceiver>& receivers = {},
    const std::vector<std::shared_ptr<const surface_stamp::Stamp>>& payload_bank = {}) {
    error.clear();
    auto fail=[&](const char *message){error=message;return false;};
    // Each admitted rigid source can contribute up to six projected faces.
    constexpr size_t max_bindings=6u*4096u;
    if (inputs.empty() || inputs.size()>max_bindings) return fail("finite source binding count outside 1..24576");
    auto result=std::make_shared<VtFiniteSources>();
    uint64_t hash=vt_finite_hash_word(14695981039346656037ull,1);
    std::map<uint64_t,std::pair<uint32_t,uint32_t>> directory;
    auto dot=[](matter::Float3 a,matter::Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    auto finite=[](matter::Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
    uint64_t payload_hash=14695981039346656037ull;
    const auto add_payload=[&](const std::shared_ptr<const surface_stamp::Stamp>& stamp) {
        if(!stamp) return fail("missing finite source payload");
        const auto& s=*stamp;
        if (s.height_projection!=1 || s.detail_min_m!=0 || s.detail_max_m!=0 || s.levels.empty() ||
            s.levels.size()>32 || !std::isfinite(s.height_min_m)||!std::isfinite(s.height_max_m)||s.height_min_m>s.height_max_m)
            return fail("finite source height has not been resolved into its projection axis");
        for (float d:s.domain) if (!std::isfinite(d)) return fail("nonfinite source domain");
        if (s.domain[2]<=0||s.domain[3]<=0) return fail("invalid finite source domain");
        size_t count=0;uint32_t w=s.levels[0].width,h=s.levels[0].height;
        if (!w||!h) return fail("empty finite source level");
        for (const auto &level:s.levels) {
            if (level.offset!=count||level.width!=w||level.height!=h||level.reserved||
                size_t(w)*h>s.pixels.size()-std::min(count,s.pixels.size())) return fail("invalid finite source directory");
            count+=size_t(w)*h;w=std::max(1u,w/2);h=std::max(1u,h/2);
        }
        if (count!=s.pixels.size()||s.levels.back().width!=1||s.levels.back().height!=1)
            return fail("incomplete finite source mip chain");
        if(directory.count(s.content_digest)) return true;
        auto found=directory.find(s.content_digest);
        if (found==directory.end()) {
            constexpr size_t max_pixels=128u*1024u*1024u/sizeof(surface_stamp::Channels);
            if (s.pixels.size()>max_pixels-result->pixel_count) return fail("finite source catalog exceeds 128 MiB");
            const uint32_t first=uint32_t(result->levels.size());
            for (auto level:s.levels) {level.offset+=uint32_t(result->pixel_count);result->levels.push_back(level);}
            result->pixel_count+=s.pixels.size();result->payloads.push_back(stamp);
            found=directory.emplace(s.content_digest,std::make_pair(first,uint32_t(s.levels.size()))).first;
        }
        payload_hash=vt_finite_hash_word(payload_hash,s.content_digest);
        return true;
    };
    for(const auto& stamp:payload_bank) if(!add_payload(stamp)) return false;
    for (const auto &input:inputs) {
        if (!input.stamp) return fail("missing finite source");
        const auto &s=*input.stamp; const auto &f=input.frame;
        const auto cross=matter::Float3{f.u.y*f.v.z-f.u.z*f.v.y,f.u.z*f.v.x-f.u.x*f.v.z,f.u.x*f.v.y-f.u.y*f.v.x};
        if (!finite(f.origin_m)||!finite(f.u)||!finite(f.v)||!finite(f.n)||!std::isfinite(input.datum_m)||
            std::abs(dot(f.u,f.u)-1)>1e-4f||std::abs(dot(f.v,f.v)-1)>1e-4f||std::abs(dot(f.n,f.n)-1)>1e-4f||
            std::abs(dot(f.u,f.v))>1e-4f||std::abs(dot(f.u,f.n))>1e-4f||std::abs(dot(f.v,f.n))>1e-4f||dot(cross,f.n)<.9999f)
            return fail("finite source requires a rigid right-handed receiver frame");
        if(!add_payload(input.stamp)) return false;
        const auto found=directory.find(s.content_digest);
        VtGpuFiniteSource b;
        auto row=[](float *dst,matter::Float3 v,float w){dst[0]=v.x;dst[1]=v.y;dst[2]=v.z;dst[3]=w;};
        row(b.origin_datum,f.origin_m,input.datum_m);row(b.u,f.u,.00005f);
        row(b.v,f.v,0);row(b.n,f.n,.9999f);
        std::copy(s.domain,s.domain+4,b.domain);
        b.levels[0]=found->second.first;b.levels[1]=found->second.second;
        result->bindings.push_back(b);
        const float lo=s.height_min_m-input.datum_m,hi=s.height_max_m-input.datum_m;
        if (!std::isfinite(lo)||!std::isfinite(hi)) return fail("invalid receiver relief range");
        if (result->bindings.size()==1) {result->height_min_m=lo;result->height_max_m=hi;}
        else {result->height_min_m=std::min(result->height_min_m,lo);result->height_max_m=std::max(result->height_max_m,hi);}
        hash=vt_finite_hash_word(hash,s.content_digest);
        // Explicit float/integer words, independent of padding or addresses.
        for (const float *r:{b.origin_datum,b.u,b.v,b.n,b.domain}) for (int c=0;c<4;++c) {
            uint32_t bits;std::memcpy(&bits,r+c,4);hash=vt_finite_hash_word(hash,bits);
        }
    }
    if (!receivers.empty()) {
        if (receivers.size()>4096) return fail("finite receiver count exceeds budget");
        result->lookup.resize(receivers.size()*6);
        for (const auto& r:receivers) {
            VtGpuFiniteSource g{};
            const auto row=[](float* d,matter::Float3 v,float w){d[0]=v.x;d[1]=v.y;d[2]=v.z;d[3]=w;};
            row(g.origin_datum,r.frame.origin_m,r.datum_m);row(g.u,r.frame.u,.00005f);
            row(g.v,r.frame.v,0);row(g.n,r.frame.n,.9999f);
            std::copy(r.domain.begin(),r.domain.end(),g.domain);
            if (!finite(r.frame.origin_m)||!finite(r.frame.u)||!finite(r.frame.v)||!finite(r.frame.n)||
                !std::isfinite(r.datum_m)||!std::isfinite(r.domain[0])||!std::isfinite(r.domain[1])||
                !std::isfinite(r.domain[2])||!std::isfinite(r.domain[3])||r.domain[2]<=0||r.domain[3]<=0)
                return fail("invalid composite receiver domain/frame");
            // A spatial index is independent of brick count. Wide footprints
            // gather multiple cells; each candidate has exactly one owner cell.
            const auto cross=matter::Float3{r.frame.u.y*r.frame.v.z-r.frame.u.z*r.frame.v.y,
                r.frame.u.z*r.frame.v.x-r.frame.u.x*r.frame.v.z,r.frame.u.x*r.frame.v.y-r.frame.u.y*r.frame.v.x};
            if(std::abs(dot(r.frame.u,r.frame.u)-1)>1e-4f || std::abs(dot(r.frame.v,r.frame.v)-1)>1e-4f ||
                std::abs(dot(r.frame.n,r.frame.n)-1)>1e-4f || std::abs(dot(r.frame.u,r.frame.v))>1e-4f ||
                dot(cross,r.frame.n)<.9999f) return fail("composite receiver frame must be rigid and right-handed");
            const double nx=std::ceil(double(r.domain[2])/.25),ny=std::ceil(double(r.domain[3])/.25);
            if (nx>65535 || ny>65535 || nx*ny>65536) return fail("finite receiver grid exceeds 65536 cells");
            const uint32_t w=uint32_t(nx),h=uint32_t(ny);
            g.levels[0]=w;g.levels[1]=h;g.levels[2]=uint32_t(result->lookup.size());
            std::vector<std::vector<std::array<uint32_t,4>>> cells(size_t(w)*h);
            size_t refs=0;std::vector<bool> used(result->bindings.size());
            for (uint32_t id:r.sources) {
                if(id>=result->bindings.size()) return fail("composite candidate out of range");
                if(used[id]) return fail("duplicate composite candidate");used[id]=true;
                const auto& b=result->bindings[id];
                float lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
                // The half-domain guard also covers every finite mip's filter
                // support. The query adds its larger physical footprint.
                for(unsigned c=0;c<4;++c) {
                    const float u=b.domain[0]+((c&1)?1.5f:-.5f)*b.domain[2];
                    const float v=b.domain[1]+((c&2)?1.5f:-.5f)*b.domain[3];
                    float rel[3];for(unsigned k=0;k<3;++k) rel[k]=b.origin_datum[k]+u*b.u[k]+v*b.v[k]-g.origin_datum[k];
                    for(unsigned a=0;a<2;++a) {
                        const float* axis=a?g.v:g.u;const float x=rel[0]*axis[0]+rel[1]*axis[1]+rel[2]*axis[2];
                        lo[a]=std::min(lo[a],x);hi[a]=std::max(hi[a],x);
                    }
                }
                const auto cell=[&](float x,unsigned a){return uint32_t(std::clamp(std::floor((x-g.domain[a])/g.domain[a+2]*(a?h:w)),0.f,float((a?h:w)-1)));};
                const uint32_t x0=cell(lo[0],0),y0=cell(lo[1],1),x1=cell(hi[0],0),y1=cell(hi[1],1);
                refs+=size_t(x1-x0+1)*(y1-y0+1);
                if(refs>1048576 || result->lookup.size()+refs+size_t(w)*h>2097152)
                    return fail("finite candidate index exceeds memory budget");
                for(uint32_t y=y0;y<=y1;++y) for(uint32_t x=x0;x<=x1;++x) cells[y*w+x].push_back({id,x0,y0,0});
            }
            result->lookup.resize(result->lookup.size()+size_t(w)*h);
            for(size_t i=0;i<cells.size();++i) {
                result->lookup[g.levels[2]+i]={uint32_t(result->lookup.size()),uint32_t(cells[i].size()),0,0};
                result->lookup.insert(result->lookup.end(),cells[i].begin(),cells[i].end());
            }
            std::memcpy(result->lookup.data()+result->receivers.size()*6,&g,sizeof(g));
            result->receivers.push_back(g);
        }
        hash=vt_finite_hash_word(hash,2);
        for (const auto& row:result->lookup) for(uint32_t word:row) hash=vt_finite_hash_word(hash,word);
    }
    result->payload_hash=payload_hash;
    result->content_hash=vt_finite_hash_word(hash,payload_hash);out=std::move(result);return true;
}
} // namespace vt
