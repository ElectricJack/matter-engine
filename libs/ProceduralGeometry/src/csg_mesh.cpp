#include "csg.h"
#include "shared_surface_math.h"
#include "mc_tables.h"
#include <array>

namespace matter::geometry::detail {
namespace {
uint64_t add(uint64_t a,uint64_t b) {
    if(b>UINT64_MAX-a) reject(data::ErrorCode::resource_limit,"CSG size overflow");
    return a+b;
}
uint64_t multiply(uint64_t a,uint64_t b) {
    if(a && b>UINT64_MAX/a) reject(data::ErrorCode::resource_limit,"CSG size overflow");
    return a*b;
}
constexpr int corners[8][3]={{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}};
constexpr int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
}

std::optional<data::MeshPrototype> mesh_csg(const CsgField &field,const data::RecipeNode &node,
    jobs::JobMonitor &monitor,jobs::ResourceUsage &usage) {
    uint64_t dims[3]; double origin[3];
    const float low[3]={field.bounds.min.x,field.bounds.min.y,field.bounds.min.z};
    const float high[3]={field.bounds.max.x,field.bounds.max.y,field.bounds.max.z};
    auto coordinate=[&](int axis,uint64_t offset) {
        return float((origin[axis]+double(offset))*double(field.spacing));
    };
    for(int axis=0;axis<3;++axis) {
        origin[axis]=std::floor(double(low[axis])/field.spacing)-2;
        double last=std::ceil(double(high[axis])/field.spacing)+2;
        double size=last-origin[axis]+1;
        if(!std::isfinite(size)||size<2||size>double(UINT32_MAX))
            reject(data::ErrorCode::resource_limit,"CSG lattice dimensions exceed addressable range");
        dims[axis]=uint64_t(size);
        require(std::isfinite(coordinate(axis,0))&&std::isfinite(coordinate(axis,dims[axis]-1))&&
            coordinate(axis,1)>coordinate(axis,0)&&coordinate(axis,dims[axis]-1)>coordinate(axis,dims[axis]-2),
            "CSG spacing cannot be represented at these coordinates");
    }
    auto count=multiply(multiply(dims[0],dims[1]),dims[2]);
    uint64_t scratch=add(field.storage,add(multiply(count,sizeof(float)),multiply(field.program.size(),sizeof(float)*2)));
    usage.working_bytes=std::max(usage.working_bytes,add(scratch,multiply(usage.vertices,256)));
    // Charge lattice admission before allocation, then field evaluation and
    // traversal as they execute. Reservations remain cumulative.
    auto minimum=usage; minimum.operations=add(usage.operations,multiply(count,field.program.size()));
    checked(monitor.checkpoint(minimum));
    usage.operations=minimum.operations;
    checked(monitor.report({0,"csg.sample"}));
    std::vector<float> samples(static_cast<size_t>(count));
    std::vector<float> values(field.program.size()),group(field.program.size());
    auto index=[&](uint64_t x,uint64_t y,uint64_t z) { return size_t((z*dims[1]+y)*dims[0]+x); };
    for(uint64_t z=0;z<dims[2];++z) for(uint64_t y=0;y<dims[1];++y) for(uint64_t x=0;x<dims[0];++x)
        samples[index(x,y,z)]=sample_csg(field,{coordinate(0,x),coordinate(1,y),coordinate(2,z)},values,group,monitor,usage);
    checked(monitor.checkpoint(usage));
    checked(monitor.report({0,"csg.mesh"}));
    data::MeshPrototype mesh; mesh.id=node.id;
    uint64_t triangles=0;
    // Count before allocating output; repeat the small table/interpolation pass
    // to emit. Triangle soup avoids legacy 16-bit indices and dedup hash caps.
    for(int pass=0;pass<2;++pass) {
        for(uint64_t z=0;z+1<dims[2];++z) for(uint64_t y=0;y+1<dims[1];++y) for(uint64_t x=0;x+1<dims[0];++x) {
            usage.operations=add(usage.operations,1);
            if((usage.operations&127)==0) checked(monitor.checkpoint(usage));
            std::array<data::Vec3,8> positions{}; float scalars[8]; int cube=0;
            for(int c=0;c<8;++c) {
                auto cx=x+corners[c][0],cy=y+corners[c][1],cz=z+corners[c][2];
                positions[c]={coordinate(0,cx),coordinate(1,cy),coordinate(2,cz)};
                scalars[c]=samples[index(cx,cy,cz)];
                if(scalars[c]<field.isovalue) cube|=1<<c;
            }
            if(edgeTable[cube]==0) continue;
            std::array<data::Vec3,12> crossings{};
            for(int edge=0;edge<12;++edge) if(edgeTable[cube]&(1<<edge)) {
                int a=edges[edge][0],b=edges[edge][1];
                crossings[edge]=mm::from_c(VertexInterpolation(mm::to_c(positions[a]),scalars[a],mm::to_c(positions[b]),scalars[b],field.isovalue));
            }
            // Explicit signed conversion makes the canonical char table safe
            // even for consumers compiling with unsigned plain char.
            for(int i=0;i<16&&static_cast<signed char>(triTable[cube][i])>=0;i+=3) {
                auto a=crossings[int(triTable[cube][i+2])],b=crossings[int(triTable[cube][i+1])],c=crossings[int(triTable[cube][i])];
                auto face=mm::cross(mm::sub(b,a),mm::sub(c,a));
                float area=mm::length(face);
                require(std::isfinite(area),"CSG triangle overflow");
                // The canonical interpolation snaps exact-isovalue endpoints.
                // Such zero-area triangles cannot enter an AssetDocument.
                if(area==0) continue;
                if(pass==0) triangles=add(triangles,1);
                else for(auto p:{a,b,c}) {
                    mesh.indices.push_back(uint32_t(mesh.positions.size())); mesh.positions.push_back(p);
                    mesh.normals.push_back(mm::scale_vec(face,1/area));
                }
            }
        }
        if(pass==0) {
            if(triangles==0) { checked(monitor.checkpoint(usage)); return {}; }
            multiply(add(usage.vertices,multiply(triangles,3)),256);
            add(usage.indices,multiply(triangles,3));
            add(usage.operations,triangles);
            add(multiply(add(usage.vertices,multiply(triangles,3)),256),scratch);
            reserve_work(triangles,scratch,monitor,usage);
            mesh.positions.reserve(size_t(triangles*3)); mesh.normals.reserve(size_t(triangles*3)); mesh.indices.reserve(size_t(triangles*3));
        }
    }
    checked(monitor.checkpoint(usage)); checked(monitor.report({0,"csg.normals"}));
    float epsilon=std::max(std::abs(field.spacing)*0.001f,1e-6f);
    for(size_t i=0;i<mesh.positions.size();++i) {
        auto p=mesh.positions[i]; float gradient[3];
        for(int axis=0;axis<3;++axis) {
            float component=axis==0?p.x:axis==1?p.y:p.z;
            float a=component-epsilon,b=component+epsilon;
            if(a==component) a=std::nextafter(component,-INFINITY);
            if(b==component) b=std::nextafter(component,INFINITY);
            auto lo=p,hi=p;
            if(axis==0) { lo.x=a;hi.x=b; } else if(axis==1) { lo.y=a;hi.y=b; } else { lo.z=a;hi.z=b; }
            gradient[axis]=(sample_csg(field,hi,values,group,monitor,usage)-sample_csg(field,lo,values,group,monitor,usage))/(b-a);
        }
        data::Vec3 normal{gradient[0],gradient[1],gradient[2]}; float length=mm::length(normal);
        if(std::isfinite(length)&&length>1e-8f) mesh.normals[i]=mm::scale_vec(normal,1/length);
    }
    require(node.materials.size()==1,"CSG requires one output material binding");
    mesh.submeshes.push_back({0,uint32_t(mesh.indices.size()),node.materials[0]}); mesh_bounds(mesh);
    checked(monitor.checkpoint(usage)); return mesh;
}
} // namespace matter::geometry::detail
