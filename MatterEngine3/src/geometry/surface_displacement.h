#pragma once
#include "geometry_hierarchy.h"
#include <array>
#include <cmath>
#include <map>

namespace geometry {
struct DisplacementConfig {
    uint32_t subdivisions = 3;
    uint32_t max_triangles = 1u << 20;
    float sample_spacing_m = .125f;
    float max_abs_height_m = .5f;
};
struct DisplacedSurface {
    MeshIndexed mesh;
    std::vector<ReceiverCorner> receivers;
};
// A reference bake for continuous fields. Uniform subdivision guarantees the
// same edge tessellation on adjacent source triangles. The sampler receives
// the ORIGINAL position/normal and an explicit world-metre footprint, never a
// previously displaced sample. Failure leaves output untouched.
using HeightSampler = std::function<bool(const ReceiverCorner&, uint32_t material,
                                         float footprint_m, float& height_m)>;
inline bool displace_surface(const MeshIndexed& source, const DisplacementConfig& config,
                             const HeightSampler& sample, DisplacedSurface& output,
                             std::string& error) {
    const auto fail=[&](const char* message){error=message;return false;};
    if(!sample || source.indices.empty() || source.indices.size()%3 ||
       source.triex.size()!=source.indices.size()/3 || config.subdivisions>8 || config.max_triangles>(1u<<20) ||
       !std::isfinite(config.sample_spacing_m) || config.sample_spacing_m<=0 ||
       !std::isfinite(config.max_abs_height_m) || config.max_abs_height_m<0)
        return fail("invalid displacement input or sampling policy");
    const uint64_t multiplier=uint64_t(1)<<(config.subdivisions*2);
    if(source.triex.size()>config.max_triangles/multiplier)return fail("displacement triangle budget exceeded");
    for(auto index:source.indices)if(index>=source.positions.size())return fail("displacement index outside source");
    const auto normalized=[](float3 p){const auto l=std::sqrt(double(p.x)*p.x+double(p.y)*p.y+double(p.z)*p.z);return l>1e-12?make_float3(float(p.x/l),float(p.y/l),float(p.z/l)):make_float3(0,0,0);};
    const auto middle=[&](const ReceiverCorner& a,const ReceiverCorner& b){
        return ReceiverCorner{make_float3((a.position.x+b.position.x)*.5f,(a.position.y+b.position.y)*.5f,(a.position.z+b.position.z)*.5f),
            normalized(make_float3(a.normal.x+b.normal.x,a.normal.y+b.normal.y,a.normal.z+b.normal.z))};
    };
    struct Triangle { std::array<ReceiverCorner,3> corners; TriEx shading; };
    std::vector<Triangle> triangles; triangles.reserve(source.triex.size()*multiplier);
    for(size_t t=0;t<source.triex.size();++t) {
        const auto& ex=source.triex[t]; const float3 normals[]={ex.N0,ex.N1,ex.N2};
        Triangle triangle;triangle.shading=ex;
        for(size_t k=0;k<3;++k)triangle.corners[k]={source.positions[source.indices[t*3+k]],normalized(normals[k])};
        triangles.push_back(triangle);
    }
    for(uint32_t level=0;level<config.subdivisions;++level) {
        std::vector<Triangle> next;next.reserve(triangles.size()*4);
        for(const auto& triangle:triangles) {
            const auto& p=triangle.corners;
            const auto a=middle(p[0],p[1]),b=middle(p[1],p[2]),c=middle(p[2],p[0]);
            const std::array<ReceiverCorner,6> receivers{p[0],p[1],p[2],a,b,c};
            const auto& ex=triangle.shading;
            const auto mid_uv=[](float2 a,float2 b){return make_float2((a.x+b.x)*.5f,(a.y+b.y)*.5f);};
            const float2 uvs[]={ex.uv0,ex.uv1,ex.uv2,mid_uv(ex.uv0,ex.uv1),mid_uv(ex.uv1,ex.uv2),mid_uv(ex.uv2,ex.uv0)};
            const float ao[]={ex.ao0,ex.ao1,ex.ao2,(ex.ao0+ex.ao1)*.5f,(ex.ao1+ex.ao2)*.5f,(ex.ao2+ex.ao0)*.5f};
            for(auto indices:{std::array<size_t,3>{0,3,5},{3,1,4},{5,4,2},{3,4,5}}) {
                auto child=ex;
                child.uv0=uvs[indices[0]];child.uv1=uvs[indices[1]];child.uv2=uvs[indices[2]];
                child.ao0=ao[indices[0]];child.ao1=ao[indices[1]];child.ao2=ao[indices[2]];
                next.push_back({{receivers[indices[0]],receivers[indices[1]],receivers[indices[2]]},child});
            }
        }
        triangles=std::move(next);
    }
    DisplacedSurface result;
    using Key=std::array<float,6>;
    std::map<Key,uint32_t> vertices;
    std::vector<float> heights;
    std::vector<uint32_t> sampled_materials;
    for(const auto& triangle:triangles) {
        auto ex=triangle.shading;
        for(const auto& receiver:triangle.corners) {
            const auto p=receiver.position,n=receiver.normal;
            const Key key{p.x,p.y,p.z,n.x,n.y,n.z};
            for(float value:key)if(!std::isfinite(value))return fail("nonfinite displacement receiver");
            if(double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z<1e-12)return fail("zero displacement receiver normal");
            auto found=vertices.find(key);
            uint32_t index;
            if(found==vertices.end()) {
                float height=0;
                if(!sample(receiver,ex.materialId,config.sample_spacing_m,height) || !std::isfinite(height) || std::abs(height)>config.max_abs_height_m)
                    return fail("displacement sample exceeds declared bounds");
                index=static_cast<uint32_t>(result.mesh.positions.size());vertices.emplace(key,index);
                result.mesh.positions.push_back(make_float3(p.x+n.x*height,p.y+n.y*height,p.z+n.z*height));
                heights.push_back(height);sampled_materials.push_back(ex.materialId);
            } else {
                index=found->second;
                if(sampled_materials[index]!=uint32_t(ex.materialId)) {
                    float height=0;
                    if(!sample(receiver,ex.materialId,config.sample_spacing_m,height) || height!=heights[index])
                        return fail("material-dependent height is discontinuous at a shared receiver");
                }
            }
            result.mesh.indices.push_back(index);result.receivers.push_back(receiver);
        }
        result.mesh.triex.push_back(ex);
    }
    // Derive shading normals from the displaced triangles; the material still
    // receives the undisplaced normal retained above.
    std::vector<std::array<double,3>> normals(result.mesh.positions.size());
    for(size_t t=0;t<result.mesh.triex.size();++t) {
        const auto a=result.mesh.positions[result.mesh.indices[t*3]],b=result.mesh.positions[result.mesh.indices[t*3+1]],c=result.mesh.positions[result.mesh.indices[t*3+2]];
        const double x1=double(b.x)-a.x,y1=double(b.y)-a.y,z1=double(b.z)-a.z,x2=double(c.x)-a.x,y2=double(c.y)-a.y,z2=double(c.z)-a.z;
        const std::array<double,3> n{y1*z2-z1*y2,z1*x2-x1*z2,x1*y2-y1*x2};
        if(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]<1e-24)return fail("displacement produced degenerate geometry");
        for(size_t k=0;k<3;++k)for(size_t axis=0;axis<3;++axis)normals[result.mesh.indices[t*3+k]][axis]+=n[axis];
    }
    for(size_t t=0;t<result.mesh.triex.size();++t) {
        auto& ex=result.mesh.triex[t];float3* n[]={&ex.N0,&ex.N1,&ex.N2};
        for(size_t k=0;k<3;++k) {const auto& v=normals[result.mesh.indices[t*3+k]];*n[k]=normalized(make_float3(float(v[0]),float(v[1]),float(v[2])));}
    }
    output=std::move(result);error.clear();return true;
}
} // namespace geometry
