#pragma once
#include "vt_module_residency_tests.h"

namespace vt_surface_connection_tests {
inline bool run_impl(matter::VulkanDevice& vk,std::string& error) {
    vt_queue_tests::Budgets budgets;
    vt::VtResidency residency;vt_queue_tests::Frames frames(vk);uint64_t serial=0;
    if(!frames.valid() || !residency.init(vk,error))return false;
    auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);
    if(!producer)return false;
    vt::VtCompositorMaterial materials[2]{};producer->set_materials(materials,2);
    residency.set_filler(std::move(producer));
    chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=atlas.atlas_h=128;
    atlas.tri_order={0,1};atlas.charts.resize(1);auto& chart=atlas.charts[0];
    chart.tangent[0]=chart.bitangent[1]=1;chart.rect_w=chart.rect_h=128;
    chart.texels_per_meter=120;chart.tri_count=2;
    const float positions[]={0,0,0,1,0,0,1,1,0,0,1,0};
    const float normals[]={0,0,1,0,0,1,0,0,1,0,0,1};
    const float uv[]={4.f/128,4.f/128,124.f/128,4.f/128,124.f/128,124.f/128,4.f/128,124.f/128};
    const uint32_t indices[]={0,1,2,0,2,3},carrier=1;
    const uint8_t weights[]={255,255,255,255};
    const std::string original="const 0.4\nconst 0.8\nconst 0\nconst 1\nconst -0.03\n"
        "material 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    const std::string edited="const 0.6\nconst 0.8\nconst 0\nconst 1\nconst -0.02\n"
        "material 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    constexpr uint64_t first=0xc011000;
    std::array<vt::VtPartContext,4> contexts;
    std::array<uint32_t,4> slots{};
    for(size_t i=0;i<contexts.size();++i) {
        auto& ctx=contexts[i];ctx.variant_hash=first+i;ctx.rung_count=1;
        ctx.positions=positions;ctx.normals=normals;ctx.surface_uvs=uv;ctx.indices=indices;
        ctx.vertex_count=4;ctx.triangle_count=2;ctx.dominant_material=carrier;
        ctx.surface_weights=weights;ctx.surface_materials=&carrier;ctx.surface_material_count=1;
        ctx.surface_tape_text=original.c_str();ctx.surface_tape_hash=42;ctx.surface_world_anchored=1;
        ctx.surface_local_to_world[0]=ctx.surface_local_to_world[5]=ctx.surface_local_to_world[10]=1;
        ctx.surface_local_to_world[3]=float(i<2 ? i : i+8);
        slots[i]=residency.register_variant(first+i,0,atlas,ctx);
        if(!slots[i])return false;
    }
    const auto advance=[&]{return frames.next(residency,++serial);};
    const auto ready=[&] {
        return vt_prepare_tests::until([&] {
            return advance() && std::all_of(slots.begin(),slots.end(),[&](uint32_t slot){
                return bool(residency.surface_boundary_source(slot));
            });
        });
    };
    if(!ready())return false;
    const vt::VtSurfaceConnectionPair a{first,first+1,7,0,0},b{first+2,first+3,7,0,0};
    residency.set_surface_walk_enabled(false);
    if(!residency.set_surface_connections({a,b},error) || !advance())return false;
    CHECK(residency.stats().surface_pairs_compiled_total==0 &&
          residency.stats().surface_table_uploads_total==0,
          "POM disabled: connection requests do not compile or upload traversal tables");
    residency.set_surface_walk_enabled(true);
    if(!advance())return false;
    const auto addresses=[&] {
        std::array<uint64_t,4> out{};
        for(size_t i=0;i<out.size();++i)out[i]=residency.surface_link_address_for_test(slots[i]);
        return out;
    };
    const auto initial=addresses();const auto before=residency.stats();
    CHECK(std::all_of(initial.begin(),initial.end(),[](uint64_t address){return address!=0;}) &&
          before.surface_link_tables==4 && before.surface_pairs_compiled_total==2 && before.surface_table_uploads_total==4,
          "surface links: two independent physical joins publish four real GPU tables");
    CHECK(before.surface_link_table_bytes==4*(sizeof(vt::VtSurfaceLinksHeaderGpu)+sizeof(vt::VtSurfaceLinkGpu)),
          "surface links: each quad exposes exactly its one shared boundary edge");
    auto reverse=a;std::swap(reverse.first,reverse.second);
    if(!residency.set_surface_connections({b,reverse,a,b},error))return false;
    for(int i=0;i<4;++i)if(!advance())return false;
    CHECK(addresses()==initial && residency.stats().surface_pairs_compiled_total==before.surface_pairs_compiled_total &&
          residency.stats().surface_table_uploads_total==before.surface_table_uploads_total &&
          residency.stats().fills_total==before.fills_total,
          "surface links: duplicate/reordered/reversed requests preserve addresses without compilation, uploads or rebaking");
    const auto retained=residency.surface_boundary_source(slots[0]);
    for(size_t i=0;i<2;++i)if(!residency.update_variant_surface(first+i,0,weights,4,&carrier,1,43,edited.c_str()))return false;
    residency.invalidate_owners({slots[0],slots[1]});
    CHECK(!residency.surface_boundary_source_current(*retained),"surface links: edit rejects the previous source revision");
    if(!ready() || !advance())return false;
    const auto changed=addresses();const auto after=residency.stats();
    CHECK(changed[0] && changed[1] && changed[0]!=initial[0] && changed[1]!=initial[1] &&
          changed[2]==initial[2] && changed[3]==initial[3],
          "surface links: a local edit replaces both affected tables and preserves unrelated GPU addresses");
    CHECK(after.surface_pairs_compiled_total==before.surface_pairs_compiled_total+1 &&
          after.surface_table_uploads_total==before.surface_table_uploads_total+2,
          "surface links: a local edit compiles only its pair and uploads only its two owners");
    CHECK(retained->inputs->surface->tape_text==original && retained->geometry.lifetime,
          "surface links: old frame leases retain the original recipe and geometry");
    vt_module_residency_tests::Sampler sampler;if(!sampler.init(vk,residency,error))return false;
    for(size_t i:{size_t(0),size_t(2)}) {
        vt_material_domain_tests::Probe probe{};probe.slots[0]=slots[i];probe.uv[0]=probe.uv[1]=.5f;
        vt_material_domain_tests::Result result{};
        if(!sampler.sample_probe(vk,residency,frames,serial,probe,result,error))return false;
        CHECK(std::abs(result.channels[0][0]-(i==0?.6f:.4f))<.006f &&
              std::abs(result.metrics[0]-(i==0?-.02f:-.03f))<.00002f,
              "surface links: native sampling sees the edited color/height and unchanged independent material");
    }
    const auto retired=residency.surface_boundary_source(slots[1]);
    residency.release_variant(first+1);
    if(!advance())return false;
    CHECK(!residency.surface_link_address_for_test(slots[0]) &&
          residency.surface_link_address_for_test(slots[2])==initial[2] &&
          residency.stats().surface_link_tables==2,
          "surface links: streaming removal disconnects the missing neighbor without recreating another join");
    for(uint32_t i=0;i<vt::kVtRetireHorizonFrames+2;++i)if(!advance())return false;
    contexts[1].surface_tape_text=edited.c_str();contexts[1].surface_tape_hash=43;
    const uint32_t replacement=residency.register_variant(first+1,0,atlas,contexts[1]);
    CHECK(replacement==slots[1],"surface links: streaming test actually reuses the retired transport slot");
    if(!replacement)return false;slots[1]=replacement;
    if(!ready() || !advance())return false;
    const auto restored=addresses();const auto current=residency.surface_boundary_source(replacement);
    CHECK(current && current->generation!=retired->generation && !residency.surface_boundary_source_current(*retired) &&
          restored[0] && restored[1] && restored[2]==initial[2] && restored[3]==initial[3],
          "surface links: republished neighbors use the new generation while unrelated tables remain shared");
    if(!residency.set_surface_connections({},error) || !advance())return false;
    CHECK(residency.stats().surface_link_tables==0 && residency.stats().surface_link_table_bytes==0 &&
          (addresses()==std::array<uint64_t,4>{}),"surface links: clearing the domain removes all published addresses");
    std::printf("VT_SURFACE_CONNECTIONS initial_pairs=2 initial_tables=4 edited_pairs=%llu edited_tables=%llu unrelated_addresses=retained streaming=remove,slot-reuse clear=empty\n",
        (unsigned long long)(after.surface_pairs_compiled_total-before.surface_pairs_compiled_total),
        (unsigned long long)(after.surface_table_uploads_total-before.surface_table_uploads_total));
    return true;
}
inline void run(matter::VulkanDevice& vk) {
    std::string error;CHECK(run_impl(vk,error),error.empty()?"native incremental surface connections":error.c_str());
    CHECK(vk.validation_error_count()==0,"surface links: zero Vulkan validation errors");
}
}
