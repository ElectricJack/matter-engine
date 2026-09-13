#include "triangle_emit.hpp"
#include "script_host.h"
#include "render/part_store.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <cstdio>

static bool near(float a, float b) { return std::abs(a-b)<1e-5f; }
static void check_attributes(const TriEx& e) {
    assert(e.materialId==7);
    assert(near(e.uv0.x,0) && near(e.uv1.x,2) && near(e.uv2.y,3));
    assert(near(e.N0.z,1) && near(e.N1.x,std::sqrt(.5f)));
    assert(near(e.N2.y,std::sqrt(.5f)));
}
int main() {
    tri_emit::TriangleBuildBuffer buffer;
    const auto emit = [&] {
        buffer.surfaceVertex(make_float3(0,0,0),make_float3(0,0,1),make_float2(0,0));
        buffer.surfaceVertex(make_float3(1,0,0),make_float3(1,0,1),make_float2(2,0));
        buffer.surfaceVertex(make_float3(0,1,0),make_float3(0,1,1),make_float2(0,3));
        buffer.endShape();
    };
    buffer.beginShape(tri_emit::ShapeType::TRIANGLES,mat4::Identity(),7); emit();
    check_attributes(buffer.tri_extra().front());
    // Core remains correct for legacy engine clients with affine transforms;
    // castle instance placement separately enforces proper rigid transforms.
    auto transform=mat4::Identity(); transform.cell[0]=-2;
    buffer.clear(); buffer.beginShape(tri_emit::ShapeType::TRIANGLES,transform,7); emit();
    const auto& mirrored=buffer.tri_extra().front();
    assert(near(mirrored.uv1.y,3) && near(mirrored.uv2.x,2));
    assert(near(mirrored.N2.x,-1/std::sqrt(5.f)) && near(mirrored.N2.z,2/std::sqrt(5.f)));
    // Strip permutations preserve each corner's attributes.
    buffer.clear(); buffer.beginShape(tri_emit::ShapeType::TRIANGLE_STRIP,mat4::Identity(),7);
    for(int i=0;i<4;++i) buffer.surfaceVertex(make_float3(float(i%2),float(i/2),0),make_float3(0,0,1),make_float2(float(i),0));
    buffer.endShape(); assert(buffer.tri_extra().size()==2);
    assert(near(buffer.tri_extra()[1].uv0.x,2) && near(buffer.tri_extra()[1].uv1.x,1));
    // A following legacy shape must not inherit normals or UVs.
    buffer.beginShape(tri_emit::ShapeType::TRIANGLES,mat4::Identity(),7);
    buffer.vertex(make_float3(0,0,0)); buffer.vertex(make_float3(1,0,0)); buffer.vertex(make_float3(0,1,0)); buffer.endShape();
    assert(near(buffer.tri_extra().back().uv0.x,0) && near(buffer.tri_extra().back().N1.x,0));

    const auto directory=std::filesystem::temp_directory_path()/"matter-surface-vertex-tests";
    std::filesystem::create_directories(directory/"parts");
    script_host::ScriptHost host; script_host::BakeOptions options;
    options.parts_dir=directory.string(); options.retain_geometry=true;
    const std::string source=R"(class SurfaceVertexProbe extends Part {
      build() { this.fill(7); this.beginShape(0);
        this.surfaceVertex(0,0,0,0,0,1,0,0);
        this.surfaceVertex(1,0,0,1,0,1,2,0);
        this.surfaceVertex(0,1,0,0,1,1,0,3); this.endShape(); }
    })";
    const auto result=host.bake_source(source,"{}",options);
    if (!result.error.ok) std::fprintf(stderr,"surface vertex bake: %s\n",result.error.message.c_str());
    assert(result.error.ok && result.geometry && result.geometry->blas);
    assert(result.geometry->blas->get_entries().size()==1);
    check_attributes(result.geometry->blas->get_entries().front()->tri_extra.front());
    for(const char* call : {"this.surfaceVertex(0,0,0,0,0,0,0,0)",
                          "this.surfaceVertex(0,0,0,0,0,1,NaN,0)",
                          "this.surfaceVertex(0,0,0,0,0,1,0)",
                          "this.surfaceVertex(0,0,0,0,0,1,'bad',0)"}) {
        auto invalid=host.bake_source(std::string("class Invalid extends Part { build(){this.beginShape(0);")+call+";this.endShape();}}","{}",options);
        assert(!invalid.error.ok && invalid.written_path.empty());
    }
    std::puts("surface_vertex_tests: PASS");
}
