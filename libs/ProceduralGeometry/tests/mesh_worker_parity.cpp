#include "mesh_worker_fixture.h"
#include "mesh_retopo.hpp"
#include <cmath>
#include <cstring>
using namespace worker_fixture;

// Only this oracle executable links the legacy backend. The fault/cancellation
// host executable and the production geometry archive do not link it.
int main(int argc,char **argv) {
    try {
        check(argc==2,"worker path required");
        auto p=product(); auto r=request(p);
        auto &params=parameters(r); params["target_ratio"]=0.75; params["iterations"]=4;
        params["seed"]=17; params["timeout_seconds"]=30;
        g::PrimitiveGeometry geometry({}, {argv[1]});
        check(advertised(geometry),"real worker unavailable");
        auto result=geometry.process(job,p,r,options(),{});
        if(!result.ok()) for(auto &e:result.diagnostics()) std::cerr<<e.message<<'\n';
        check(result.ok(),"real worker failed small closed sphere");
        check(s::validate(*result.value()).ok(),"real result invalid");

        // The same legacy bake path input conversion: triangle soup, attributes,
        // canonical from_tri welding, canonical retopo with threads pinned to 1.
        const auto &source=p.document.meshes[0];
        std::vector<Tri> tris(source.indices.size()/3);
        std::vector<TriEx> extra(tris.size());
        auto f=[](d::Vec3 v) { return make_float3(v.x,v.y,v.z); };
        for(size_t t=0;t<tris.size();++t) {
            const auto a=source.indices[t*3],b=source.indices[t*3+1],c=source.indices[t*3+2];
            auto &tri=tris[t]; std::memset(&tri,0,sizeof tri);
            tri.vertex0=f(source.positions[a]); tri.vertex1=f(source.positions[b]); tri.vertex2=f(source.positions[c]);
            tri.centroid=(tri.vertex0+tri.vertex1+tri.vertex2)*(1.0f/3);
            auto &e=extra[t]; e.N0=f(source.normals[a]); e.N1=f(source.normals[b]); e.N2=f(source.normals[c]);
            e.uv0=e.uv1=e.uv2=make_float2(0.25f,0.75f); e.tint=make_float4(1,1,1,0);
            e.ao0=e.ao1=e.ao2=1; e.materialId=0;
        }
        auto input=from_tri(tris,&extra);
        RetopoOptions legacy; legacy.target_ratio=0.75f; legacy.iterations=4; legacy.seed=17;
        legacy.timeout_seconds=30; legacy.threads=1;
        auto expected=retopo(input,legacy);
        check(expected.ok,"legacy oracle failed");
        const auto &actual=result.value()->document.meshes[0];
        check(actual.indices.size()==expected.mesh.indices.size(),"legacy topology count mismatch");
        auto near=[](float a,float b) { check(std::abs(a-b)<=1e-6f+1e-6f*std::abs(b),"legacy coordinate/attribute mismatch"); };
        for(size_t k=0;k<actual.indices.size();++k) {
            const auto index=actual.indices[k]; auto a=actual.positions[index]; auto b=expected.mesh.positions[expected.mesh.indices[k]];
            near(a.x,b.x); near(a.y,b.y); near(a.z,b.z);
            const auto &e=expected.mesh.triex[k/3];
            const float3 ns[]={e.N0,e.N1,e.N2}; const auto n=ns[k%3];
            const double length=std::sqrt(double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z);
            near(actual.normals[index].x,float(n.x/length)); near(actual.normals[index].y,float(n.y/length));
            near(actual.normals[index].z,float(n.z/length));
            near(actual.uv_sets[0][index].x,0.25f); near(actual.uv_sets[0][index].y,0.75f);
        }
        check(actual.submeshes.size()==1 && actual.submeshes[0].material==source.submeshes[0].material,"material parity");
        auto again=geometry.process(job,p,r,options(),{});
        check(again.ok(),"second worker invocation failed");
        std::string first,second;
        check(d::encode(result.value()->document,first).ok() && d::encode(again.value()->document,second).ok() && first==second,
            "isolated invocations differ");
        std::cout<<"MESH-WORKER-PARITY: legacy closed sphere, "<<tris.size()<<" input / "<<actual.indices.size()/3
            <<" output triangles; oriented corners, normals, UV, material and repeated result pass\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
