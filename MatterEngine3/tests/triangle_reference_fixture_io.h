#pragma once
// Diagnostic source-mesh interchange. Stores each original mesh once, plus
// exact placements; never expands hundreds of millions of placed triangles.
// This is not a production cache format. Colors resolve the cached material
// base color and tint, normals are the original per-vertex shading normals.
#include "sparse_hierarchy_fixture_io.h"

namespace triangle_reference_fixture {
struct Triangle { float position[9],normal[9],color[3]; };
struct SurfaceLevel { float at=0,error_upper=-1; std::vector<Triangle> triangles; };
struct Prototype {
    uint64_t key=0; std::vector<Triangle> triangles;
    bool prefer_surface=false;
    std::vector<SurfaceLevel> coarser_surfaces;
};
struct Placement { float transform[16]; uint32_t prototype; };
struct Scene { uint64_t root_key=0; std::vector<Prototype> prototypes; std::vector<Placement> placements; };
static_assert(sizeof(Triangle)==84 && sizeof(Placement)==68);
inline bool write(std::ostream& f,const Scene& scene) {
    using sparse_hierarchy_fixture::write;
    write(f,uint64_t(0x3346455249525453ull)); // STRIREF3
    write(f,uint32_t(0x01020304)); write(f,scene.root_key);
    write(f,uint32_t(scene.prototypes.size())); write(f,uint32_t(scene.placements.size()));
    for(const auto& p:scene.prototypes) {
        write(f,p.key); write(f,uint32_t(p.triangles.size()));
        f.write(reinterpret_cast<const char*>(p.triangles.data()),std::streamsize(p.triangles.size()*sizeof(Triangle)));
        write(f,uint32_t(p.prefer_surface));write(f,uint32_t(p.coarser_surfaces.size()));
        for(const auto& l:p.coarser_surfaces) {
            write(f,l.at);write(f,l.error_upper);write(f,uint32_t(l.triangles.size()));
            f.write(reinterpret_cast<const char*>(l.triangles.data()),std::streamsize(l.triangles.size()*sizeof(Triangle)));
        }
    }
    f.write(reinterpret_cast<const char*>(scene.placements.data()),std::streamsize(scene.placements.size()*sizeof(Placement)));
    return bool(f);
}
inline bool read(std::istream& f,Scene& out,std::string& error) {
    using sparse_hierarchy_fixture::read;
    const auto fail=[&](const char* s) { error=s; return false; };
    uint64_t magic=0; uint32_t endian=0,prototypes=0,placements=0; Scene scene;
    if(!read(f,magic) || (magic!=0x3146455249525453ull && magic!=0x3246455249525453ull && magic!=0x3346455249525453ull) || !read(f,endian) || endian!=0x01020304 ||
       !read(f,scene.root_key) || !read(f,prototypes) || !read(f,placements) ||
       !prototypes || prototypes>1024 || !placements || placements>1000000)
        return fail("invalid triangle reference header");
    uint64_t total=0;
    const auto mesh=[&](std::vector<Triangle>& triangles)->bool {
        uint32_t count=0;
        if(!read(f,count) || !count || count>5000000-total)
            return fail("invalid triangle reference mesh count");
        total+=count; triangles.resize(count);
        if(!f.read(reinterpret_cast<char*>(triangles.data()),std::streamsize(count*sizeof(Triangle))))
            return fail("truncated triangle reference mesh");
        for(const auto& t:triangles) {
            for(float v:t.position) if(!std::isfinite(v)) return fail("nonfinite reference position");
            for(float v:t.normal) if(!std::isfinite(v)) return fail("nonfinite reference normal");
            for(float v:t.color) if(!std::isfinite(v) || v<0 || v>1) return fail("invalid reference color");
        }
        return true;
    };
    for(uint32_t i=0;i<prototypes;++i) {
        Prototype p;
        if(!read(f,p.key) || !mesh(p.triangles)) return false;
        if(magic!=0x3146455249525453ull) {
            uint32_t flags=0,levels=0;
            if(!read(f,flags) || flags>1 || !read(f,levels) || levels>31 || (levels&&!flags))
                return fail("invalid reference surface ladder");
            p.prefer_surface=flags!=0;
            float previous=0;
            for(uint32_t l=0;l<levels;++l) {
                SurfaceLevel level;
                if(!read(f,level.at) || !std::isfinite(level.at) || level.at<=previous ||
                   (magic==0x3346455249525453ull && (!read(f,level.error_upper) || !std::isfinite(level.error_upper) ||
                    (level.error_upper<0 && level.error_upper!=-1))) || !mesh(level.triangles))
                    return fail("invalid reference surface LOD");
                previous=level.at;p.coarser_surfaces.push_back(std::move(level));
            }
        }
        scene.prototypes.push_back(std::move(p));
    }
    scene.placements.resize(placements);
    if(!f.read(reinterpret_cast<char*>(scene.placements.data()),std::streamsize(placements*sizeof(Placement))))
        return fail("truncated triangle reference placements");
    for(const auto& p:scene.placements) {
        if(p.prototype>=prototypes) return fail("invalid triangle reference prototype");
        for(float v:p.transform) if(!std::isfinite(v)) return fail("nonfinite reference transform");
        if(p.transform[12]!=0 || p.transform[13]!=0 || p.transform[14]!=0 || p.transform[15]!=1)
            return fail("non-affine reference transform");
    }
    if(f.peek()!=std::char_traits<char>::eof()) return fail("trailing triangle reference data");
    out=std::move(scene); error.clear(); return true;
}
} // namespace triangle_reference_fixture
