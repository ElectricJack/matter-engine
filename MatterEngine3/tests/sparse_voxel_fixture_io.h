#pragma once
// Diagnostic interchange for CPU-bake/GPU-render tests only. This text format
// is deliberately not a published runtime asset format or cache contract.
#include "sparse_voxel_bake.h"
#include "material_registry.h"
#include <fstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

// Match material_common.glsl's base-color/tint blend. This diagnostic source
// reader does not yet evaluate surface-detail atlases, transmission or normals.
inline sparse_voxel::SurfaceSample sparse_source_material(int material_id,const std::array<float,4>& tint) {
    const MaterialDef* material=MaterialRegistryGet(material_id);
    const float blend=std::max(0.0f,std::min(1.0f,tint[3]));
    return {{material->albedo[0]*(1-blend)+tint[0]*blend,
             material->albedo[1]*(1-blend)+tint[1]*blend,
             material->albedo[2]*(1-blend)+tint[2]*blend},material->opacity};
}

inline bool write_sparse_source_fixture(const char* path,const std::vector<sparse_voxel::Triangle>& triangles) {
    std::ofstream f(path);
    f << std::setprecision(17) << "SPARSE_SOURCE_DIAGNOSTIC_1\n" << triangles.size() << '\n';
    for(const auto& t:triangles) {
        for(const auto& p:t.positions) f << p.x << ' ' << p.y << ' ' << p.z << ' ';
        for(const auto& uv:t.uv) f << uv.x << ' ' << uv.y << ' ';
        f << t.surface.albedo.x << ' ' << t.surface.albedo.y << ' ' << t.surface.albedo.z << ' ' << t.surface.coverage << '\n';
    }
    f.close(); return bool(f);
}

inline bool read_sparse_source_fixture(const char* path,std::vector<sparse_voxel::Triangle>& out,std::string& error) {
    std::ifstream f(path); std::string magic; size_t count=0;
    if(!(f >> magic >> count) || magic!="SPARSE_SOURCE_DIAGNOSTIC_1" || !count || count>2000000) {
        error="invalid sparse source diagnostic header"; return false;
    }
    std::vector<sparse_voxel::Triangle> triangles(count);
    for(auto& t:triangles) {
        for(auto& p:t.positions) f >> p.x >> p.y >> p.z;
        for(auto& uv:t.uv) f >> uv.x >> uv.y;
        f >> t.surface.albedo.x >> t.surface.albedo.y >> t.surface.albedo.z >> t.surface.coverage;
        bool finite=std::isfinite(t.surface.albedo.x) && std::isfinite(t.surface.albedo.y) &&
            std::isfinite(t.surface.albedo.z) && std::isfinite(t.surface.coverage);
        for(const auto& p:t.positions) finite=finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        for(const auto& uv:t.uv) finite=finite && std::isfinite(uv.x) && std::isfinite(uv.y);
        if(!f || !finite || t.surface.coverage<0 || t.surface.coverage>1) {
            error="invalid or truncated sparse source diagnostic triangle"; return false;
        }
    }
    out=std::move(triangles); return true;
}

inline bool write_sparse_fixture(const char* path, const sparse_voxel::Asset& a) {
    std::ofstream f(path);
    f << std::setprecision(17) << "SPARSE_VOXEL_DIAGNOSTIC_3\n"
      << a.origin.x << ' ' << a.origin.y << ' ' << a.origin.z << ' '
      << a.cell_size << ' ' << a.bricks.size() << ' ' << a.cells.size() << '\n';
    for (const auto& b:a.bricks)
        f << b.coord[0] << ' ' << b.coord[1] << ' ' << b.coord[2] << ' '
          << b.mask << ' ' << b.first_cell << '\n';
    for (const auto& c:a.cells) {
        f << c.area;
        for (auto v:c.albedo_area) f << ' ' << v;
        for (auto v:c.normal_area) f << ' ' << v;
        for (auto v:c.normal_second_area) f << ' ' << v;
        f << ' ' << c.has_support;
        for(auto v:c.support_min) f << ' ' << v;
        for(auto v:c.support_max) f << ' ' << v;
        for(auto v:c.plane) f << ' ' << v;
        f << ' ' << c.has_projection;for(auto v:c.projected_area) f << ' ' << v;
        f << '\n';
    }
    f.close();
    return bool(f);
}

inline bool read_sparse_fixture(const char* path,sparse_voxel::Asset& out,std::string& error) {
    std::ifstream f(path);
    std::string magic;
    sparse_voxel::Asset a;
    uint64_t bricks=0,cells=0;
    if (!(f >> magic) || (magic!="SPARSE_VOXEL_DIAGNOSTIC_1" && magic!="SPARSE_VOXEL_DIAGNOSTIC_2" && magic!="SPARSE_VOXEL_DIAGNOSTIC_3") ||
        !(f >> a.origin.x >> a.origin.y >> a.origin.z >> a.cell_size >> bricks >> cells) ||
        bricks>(1u<<20) || cells>(1u<<20)) {
        error="invalid sparse voxel diagnostic fixture header"; return false;
    }
    a.bricks.resize(size_t(bricks)); a.cells.resize(size_t(cells));
    for (auto& b:a.bricks) f >> b.coord[0] >> b.coord[1] >> b.coord[2] >> b.mask >> b.first_cell;
    for (auto& c:a.cells) {
        f >> c.area;
        for (auto& v:c.albedo_area) f >> v;
        for (auto& v:c.normal_area) f >> v;
        for (auto& v:c.normal_second_area) f >> v;
        if(magic!="SPARSE_VOXEL_DIAGNOSTIC_1") {
            f >> c.has_support;
            for(auto& v:c.support_min) f >> v;
            for(auto& v:c.support_max) f >> v;
            for(auto& v:c.plane) f >> v;
        }
        if(magic=="SPARSE_VOXEL_DIAGNOSTIC_3") {f >> c.has_projection;for(auto& v:c.projected_area) f >> v;}
    }
    if (!f || !sparse_voxel::validate(a,error)) {
        if (error.empty()) error="truncated sparse voxel diagnostic fixture";
        return false;
    }
    out=std::move(a); return true;
}
