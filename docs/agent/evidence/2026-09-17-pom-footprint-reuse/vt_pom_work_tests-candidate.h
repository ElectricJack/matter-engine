#pragma once
#include "vt_module_residency_tests.h"

namespace vt_pom_work_tests {
inline bool run_impl(matter::VulkanDevice& vk,std::string& error) {
    vt_queue_tests::Budgets budgets;
    vt::VtResidency residency;vt_queue_tests::Frames frames(vk);uint64_t serial=0;
    if(!frames.valid() || !residency.init(vk,error))return false;
    auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);
    if(!producer)return false;
    vt::VtCompositorMaterial materials[2]{};producer->set_materials(materials,2);
    residency.set_filler(std::move(producer));
    const std::string tape="const 0.4\nconst 0.8\nconst 0\nconst 1\nconst -0.03\n"
        "material 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    vt::VtPeriodicDomain domain;std::shared_ptr<const vt::VtPartSnapshot> input;
    if(!vt::vt_make_periodic_domain(frame,{2,1},128,domain,error) ||
       !vt::vt_make_periodic_material(domain,{},tape,1,input,error))return false;
    vt::VtMaterialModuleLease module;if(!residency.acquire_material_module(input,module,error))return false;
    if(!vt_prepare_tests::until([&]{return frames.next(residency,++serial) &&
        residency.material_module_binding(module).slot;}))return false;
    vt_module_residency_tests::Sampler sampler;if(!sampler.init(vk,residency,error))return false;
    vt_material_domain_tests::Probe probe{};probe.slots[0]=residency.material_module_binding(module).slot;
    probe.slots[3]=0xa0000000u;probe.derivatives[2]=1;probe.derivatives[3]=1.f/128;
    using Result=vt_material_domain_tests::Result;
    const auto sample=[&](float u,float footprint,float fade,Result& result) {
        probe.uv[0]=u;probe.uv[1]=.5f;probe.uv[2]=0;probe.uv[3]=footprint;
        probe.slots[2]=vt_material_domain_tests::bits(fade);
        return sampler.sample_probe(vk,residency,frames,serial,probe,result,error);
    };
    Result near_result;if(!sample(.5f,.001f,1.f,near_result))return false;
    CHECK(near_result.pom[1]==1 && std::abs(near_result.pom[0]-.03f)<3e-6f && near_result.single_lookup[0]>0,
        "POM work: nearby native surface retains accurate displaced hit and footprint checks");
    CHECK(near_result.single_lookup[0]==1,
        "POM work: fixed-UV ray validates its immutable bilinear quad once, including refinement");
    // Move through multiple texel quads. Reusing the initial quad forever
    // would skip required checks; repeating it for each refinement wastes work.
    probe.derivatives[0]=1.f;probe.derivatives[1]=.6f;
    Result oblique;if(!sample(.49f,.001f,1.f,oblique))return false;
    CHECK(oblique.pom[1]==1 && std::abs(oblique.pom[0]-.03f)<3e-6f &&
          oblique.single_lookup[0]>1 && oblique.single_lookup[0]<30,
        "POM work: moving ray revalidates distinct quads and retains its precise height crossing");
    probe.derivatives[0]=probe.derivatives[1]=0;
    std::printf("VT_POM_WORK oblique_t=%.9f oblique_footprints=%.0f\n",
        oblique.pom[0],oblique.single_lookup[0]);
    float max_footprints=0,max_connected=0;
    for(float u:{.001f,.5f,.999f})for(float footprint:{.75f,2.f}) {
        Result far_result;if(!sample(u,footprint,1.f,far_result))return false;
        CHECK(far_result.pom[0]==0 && far_result.pom[1]==6,"POM work: unresolved native surface retains flat fallback");
        max_footprints=std::max(max_footprints,far_result.single_lookup[0]);
        max_connected=std::max(max_connected,far_result.single_lookup[1]);
    }
    CHECK(max_footprints==0 && max_connected==0,
        "POM work: unresolved pixels perform no bilinear chart validation or connected geometry search");
    Result faded;if(!sample(.5f,.001f,0.f,faded))return false;
    CHECK(faded.pom[0]==0 && faded.pom[1]==6 && faded.single_lookup[0]==0 && faded.single_lookup[1]==0,
        "POM work: distance-faded pixels skip chart and geometry work");
    // A finite chart with padding exercises the connected-surface fallback,
    // which periodic coverage cannot reach. Geometry and UVs agree with the
    // probe's local point (u*2, v, 0), including the four-texel chart gutter.
    chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=128;atlas.atlas_h=64;
    atlas.tri_order={0,1};atlas.charts.resize(1);auto& c=atlas.charts[0];
    c.origin[0]=c.origin[1]=.0625f;c.tangent[0]=c.bitangent[1]=1;
    c.rect_w=128;c.rect_h=64;c.texels_per_meter=64;c.tri_count=2;
    const float positions[]={.0625f,.0625f,0,1.9375f,.0625f,0,1.9375f,.9375f,0,.0625f,.9375f,0};
    const float normals[]={0,0,1,0,0,1,0,0,1,0,0,1};
    const float uv[]={.03125f,.0625f,.96875f,.0625f,.96875f,.9375f,.03125f,.9375f};
    const uint32_t indices[]={0,1,2,0,2,3},carrier=1;
    const uint8_t weights[]={255,255,255,255};
    constexpr uint64_t finite_hash=0xfa1de001;
    vt::VtPartContext ctx;ctx.variant_hash=finite_hash;ctx.rung_count=1;
    ctx.positions=positions;ctx.normals=normals;ctx.surface_uvs=uv;ctx.indices=indices;
    ctx.vertex_count=4;ctx.triangle_count=2;ctx.dominant_material=carrier;
    ctx.surface_weights=weights;ctx.surface_materials=&carrier;ctx.surface_material_count=1;
    ctx.surface_tape_text=tape.c_str();ctx.surface_tape_hash=finite_hash;
    const uint32_t finite_slot=residency.register_variant(finite_hash,0,atlas,ctx);
    if(!finite_slot || !vt_prepare_tests::until([&]{return frames.next(residency,++serial) &&
        residency.slot_active(finite_slot);}))return false;
    probe.slots[0]=finite_slot;probe.derivatives[3]=1.f/64;
    Result boundary;if(!sample(4.01f/128,2.f,1.f,boundary))return false;
    CHECK(boundary.pom[0]==0 && boundary.pom[1]==6 && boundary.single_lookup[0]==0 && boundary.single_lookup[1]==0,
        "POM work: unresolved finite chart boundary skips connected geometry search");
    std::printf("VT_POM_WORK near_t=%.9f near_footprints=%.0f far_footprints=%.0f far_connected=%.0f faded_footprints=%.0f\n",
        near_result.pom[0],near_result.single_lookup[0],max_footprints,max_connected,faded.single_lookup[0]);
    std::printf("VT_POM_WORK finite_boundary_t=%.9f status=%.0f footprints=%.0f connected=%.0f\n",
        boundary.pom[0],boundary.pom[1],boundary.single_lookup[0],boundary.single_lookup[1]);
    return true;
}
inline void run(matter::VulkanDevice& vk) {
    std::string error;CHECK(run_impl(vk,error),error.empty()?"native POM work probe":error.c_str());
    CHECK(vk.validation_error_count()==0,"POM work: zero Vulkan validation errors");
}
}
