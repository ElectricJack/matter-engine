#pragma once
#include "vt_module_residency_tests.h"
#include "render/vt_enrich.h"

namespace vt_receiver_material_tests {
class OcclusionWriter final : public vt::VtPageEnricher {
public:
    explicit OcclusionWriter(std::unique_ptr<vt::VtEnricher> writer):writer_(std::move(writer)) {}
    uint64_t refuse_owner=0;uint32_t refusals=0;
    std::function<void()> after_write;
    bool supports_separate_occlusion() const override {return true;}
    uint32_t sample_count() const override {return writer_->sample_count();}
    float max_footprint_meters() const override {return writer_->max_footprint_meters();}
    void invalidate_part(uint64_t hash) override {writer_->invalidate_part(hash);}
    void release_preparation(const vt::VtPreparationKey& key) override {writer_->release_preparation(key);}
    void enrich(VkCommandBuffer cmd,const vt::VtEnrichRequest* requests,size_t count) override {
        writer_->enrich(cmd,requests,count);
        for(size_t i=0;i<count;++i)if(requests[i].variant_hash==refuse_owner &&
            requests[i].out_enriched && *requests[i].out_enriched) {*requests[i].out_enriched=false;++refusals;}
        if(after_write){auto callback=std::move(after_write);after_write={};callback();}
    }
private:
    std::unique_ptr<vt::VtEnricher> writer_;
};

inline bool run_occlusion_impl(matter::VulkanDevice& vk,std::string& error) {
    if(!vk.ray_tracing_available()){error="receiver occlusion requires native ray tracing";return false;}
    vt_queue_tests::Budgets budgets;matter::vt_residency_budgets().enrich_per_frame=2;
    struct Settings {
        matter::VtEnrichSettings previous;
        Settings(){matter::ensure_vt_enrich_env_applied();previous=matter::vt_enrich_settings();matter::vt_enrich_settings()={};}
        ~Settings(){matter::vt_enrich_settings()=previous;}
    } settings;
    vt::VtResidency residency;vt_queue_tests::Frames frames(vk);uint64_t serial=0;
    auto native=vt::VtEnricher::create(vk,VK_NULL_HANDLE,error);if(!native)return false;
    auto wrapper=std::make_unique<OcclusionWriter>(std::move(native));auto* writer=wrapper.get();
    constexpr uint64_t hashes[]={0xa001,0xa002};writer->refuse_owner=hashes[1];
    residency.set_enricher(std::move(wrapper));
    if(!frames.valid() || !residency.init(vk,error))return false;
    auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);if(!producer)return false;
    vt::VtCompositorMaterial materials[2]{};producer->set_materials(materials,2);residency.set_filler(std::move(producer));
    const auto advance=[&]{return frames.next(residency,++serial);};
    const std::string tape="const 0.6\nconst 0.4\nconst 0\nconst 1\nconst -0.04\nconst 0.8\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r5 r4 -0.08 0\n";
    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    vt::VtPeriodicDomain domain;std::shared_ptr<const vt::VtPartSnapshot> module_input;
    if(!vt::vt_make_periodic_domain(frame,{1,1},256,domain,error) ||
       !vt::vt_make_periodic_material(domain,{},tape,1,module_input,error))return false;
    vt::VtMaterialModuleLease module;if(!residency.acquire_material_module(module_input,module,error))return false;
    vt::VtReceiverMaterialChart mapping;mapping.module=module;mapping.frame=frame;
    uint32_t slots[2]{};
    for(int i=0;i<2;++i) {
        // Both receivers share one material. Only the second has a close
        // overhead plate in its own BLAS; this is actual traced occlusion.
        chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=1280;atlas.atlas_h=640;
        const uint32_t faces=i?2:1;atlas.charts.resize(faces);atlas.tri_order={0,1};
        if(i){atlas.tri_order.push_back(2);atlas.tri_order.push_back(3);}
        float positions[24]{},normals[24]{},uv[16]{};uint32_t indices[12]{};uint8_t weights[8]{};
        for(uint32_t face=0;face<faces;++face) {
            auto& c=atlas.charts[face];c={};c.origin[2]=face*.01f;c.tangent[0]=c.bitangent[1]=1;
            c.rect_x=face*640;c.rect_w=c.rect_h=640;c.texels_per_meter=128;c.first_tri=face*2;c.tri_count=2;
            for(uint32_t j=0;j<4;++j) {
                const uint32_t v=face*4+j;const float x=(j==1||j==2)?4.f:0.f,y=j>=2?4.f:0.f;
                positions[v*3]=x;positions[v*3+1]=y;positions[v*3+2]=face*.01f;normals[v*3+2]=1;
                uv[v*2]=(c.rect_x+4+x*128)/1280;uv[v*2+1]=(4+y*128)/640;weights[v]=255;
            }
            const uint32_t corners[]={0,1,2,0,2,3};
            for(uint32_t j=0;j<6;++j)indices[face*6+j]=face*4+corners[j];
        }
        const uint32_t carrier=1;vt::VtPartContext ctx;ctx.variant_hash=hashes[i];ctx.rung_count=1;
        ctx.positions=positions;ctx.normals=normals;ctx.surface_uvs=uv;ctx.indices=indices;
        ctx.vertex_count=faces*4;ctx.triangle_count=faces*2;ctx.dominant_material=1;
        ctx.surface_weights=weights;ctx.surface_materials=&carrier;ctx.surface_material_count=1;
        ctx.surface_tape_text=tape.c_str();ctx.surface_tape_hash=hashes[i];
        slots[i]=residency.register_variant(hashes[i],0,atlas,ctx);
        if(!slots[i] || !residency.bind_receiver_materials(hashes[i],0,{mapping},error))return false;
    }
    if(!vt_prepare_tests::until([&]{return advance() && residency.slot_active(slots[0]) &&
        residency.slot_active(slots[1]) && residency.material_module_binding(module).slot;}))return false;
    if(!advance())return false;
    const auto base_allocations=residency.stats().material_pages;
    const auto page=[&](int i){return residency.resident_page_slot_for_test(slots[i],{0,1,1});};
    vt::VtFeedbackRequest demands[]={{slots[0]-1,0,1,1},{slots[1]-1,0,1,1}};
    residency.inject_feedback_for_test(demands,2);
    if(!vt_prepare_tests::until([&]{return advance() && page(0)!=UINT32_MAX && page(1)!=UINT32_MAX;}))return false;
    CHECK(residency.coverage_only_page_for_test(page(0)) && residency.coverage_only_page_for_test(page(1)) &&
          residency.stats().material_pages==base_allocations,"receiver AO: shipping enrichment budget preserves shared coverage-only interiors");
    vt_module_residency_tests::Sampler sampler;if(!sampler.init(vk,residency,error))return false;
    const auto probe=[&](int i) {
        vt_material_domain_tests::Probe p{};p.slots[0]=slots[i];p.slots[3]=0x80000000u;
        p.uv[0]=(4+1.5f*128)/1280;p.uv[1]=(4+1.5f*128)/640;
        p.derivatives[2]=1;p.derivatives[3]=1.f/128;return p;
    };
    const auto sample=[&](int i,vt_material_domain_tests::Result& result) {
        return sampler.sample_probe(vk,residency,frames,serial,probe(i),result,error);
    };
    vt_material_domain_tests::Result rejected{},clear{},occluded{};
    for(int i=0;i<3;++i)if(!sample(1,rejected))return false;
    CHECK(writer->refusals>0 && !residency.occlusion_address_for_test(page(1)) &&
          std::abs(rejected.channels[2][0]-.8f)<.01f,"receiver AO: refused GPU writes cannot expose an unpublished factor");
    writer->refuse_owner=0;
    if(!vt_prepare_tests::until([&]{return advance() && residency.occlusion_address_for_test(page(0)) &&
        residency.occlusion_address_for_test(page(1));}) || !sample(0,clear) || !sample(1,occluded))return false;
    CHECK(std::abs(clear.channels[2][0]-.8f)<.01f && occluded.channels[2][0]<.4f && occluded.channels[2][0]>.10f,
          "receiver AO: actual geometry occludes one receiver while the shared material stays unchanged");
    for(int channel:{0,1,4})CHECK(std::memcmp(clear.channels[channel],occluded.channels[channel],sizeof(clear.channels[channel]))==0,
          "receiver AO: albedo, normal and height remain shared and identical");
    CHECK(clear.channels[2][1]==occluded.channels[2][1] && clear.channels[2][2]==occluded.channels[2][2] &&
          clear.pom[0]==occluded.pom[0],"receiver AO: roughness, metalness and POM are unaffected");
    const uint64_t address=residency.occlusion_address_for_test(page(1));const float ao=occluded.channels[2][0];
    auto shifted=mapping;shifted.phase={.23f,.37f};
    if(!residency.bind_receiver_materials(hashes[1],0,{shifted},error) || !sample(1,occluded))return false;
    CHECK(residency.occlusion_address_for_test(page(1))==address && occluded.channels[2][0]==ao,
          "receiver AO: module phase edits reuse the independent geometric factor without compounding it");
    const auto drops=residency.stats().enrich_dropped_total;
    residency.invalidate_all_content();
    if(!advance())return false;
    writer->after_write=[&]{residency.invalidate_all_content();};
    if(!vt_prepare_tests::until([&]{return advance() && !writer->after_write;}))return false;
    CHECK(residency.stats().enrich_dropped_total>drops,"receiver AO: an input edit during generation rejects stale factors");
    if(!vt_prepare_tests::until([&]{return advance() && residency.occlusion_address_for_test(page(1));}) || !sample(1,occluded))return false;
    CHECK(std::abs(occluded.channels[2][0]-ao)<1e-6f && residency.stats().material_pages==base_allocations,
          "receiver AO: regenerated factors are deterministic and never duplicate module material pages");
    const auto allocated=residency.stats().occlusion_allocated_bytes;
    CHECK(allocated>0 && residency.stats().occlusion_retained_pages>=residency.stats().occlusion_pages,
          "receiver AO: memory accounting includes live and retired GPU leases");
    module.reset();mapping.module.reset();shifted.module.reset();
    for(auto hash:hashes)residency.release_variant(hash);
    CHECK(residency.stats().occlusion_pages==0 && residency.stats().occlusion_retained_pages>0,
          "receiver AO: release retires factors without freeing earlier GPU readers");
    for(uint32_t i=0;i<vt::kVtRetireHorizonFrames+2;++i)if(!advance())return false;
    CHECK(residency.stats().occlusion_retained_pages==0 && residency.stats().occlusion_allocated_bytes==0,
          "receiver AO: final retirement returns every sparse allocation");
    std::printf("VT_RECEIVER_OCCLUSION clear=%.6f occluded=%.6f refused=%u allocated_before_release=%llu default_budget=2\n",
        clear.channels[2][0],ao,writer->refusals,static_cast<unsigned long long>(allocated));
    return true;
}
// Exercise the shipped compositor, including failures AFTER it writes scratch
// pixels. The control owner deliberately keeps the ordinary full-page path.
class CoverageWriter final : public vt::VtPageFiller {
public:
    explicit CoverageWriter(std::unique_ptr<vt::VtCompositor> writer):writer_(std::move(writer)) {}
    uint64_t full_owner=0,refuse_owner=0;
    uint32_t refusals=0;
    void begin_preparation_frame() override {writer_->begin_preparation_frame();}
    bool prepare(const vt::VtPreparationKey& key,const std::shared_ptr<const vt::VtPartSnapshot>& inputs) override {
        return writer_->prepare(key,inputs);
    }
    void release_preparation(const vt::VtPreparationKey& key) override {writer_->release_preparation(key);}
    void invalidate_surface(const vt::VtPreparationKey& key) override {writer_->invalidate_surface(key);}
    void fill(VkCommandBuffer cmd,const vt::VtFillRequest* requests,size_t count) override {
        std::vector<vt::VtFillRequest> copies(requests,requests+count);
        for(auto& request:copies)if(request.variant_hash==full_owner)request.coverage_only=false;
        writer_->fill(cmd,copies.data(),copies.size());
        for(auto& request:copies)if(request.variant_hash==refuse_owner && !request.coverage_only &&
            request.out_filled && *request.out_filled) { *request.out_filled=false;++refusals; }
    }
private:
    std::unique_ptr<vt::VtCompositor> writer_;
};

inline bool run_coverage_impl(matter::VulkanDevice& vk,std::string& error) {
    vt_queue_tests::Budgets budgets;
    vt::VtResidency residency;vt_queue_tests::Frames frames(vk);uint64_t serial=0;
    if(!frames.valid() || !residency.init(vk,error))return false;
    auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);
    if(!producer)return false;
    vt::VtCompositorMaterial materials[2]{};producer->set_materials(materials,2);
    auto* compositor=producer.get();
    auto wrapper=std::make_unique<CoverageWriter>(std::move(producer));auto* writer=wrapper.get();
    constexpr uint64_t hashes[]={0xfeed201,0xfeed202};writer->full_owner=hashes[1];
    residency.set_filler(std::move(wrapper));
    const auto advance=[&]{return frames.next(residency,++serial);};
    const std::string fallback="const 0.1\nconst 0.7\nconst 0\nconst 1\nconst -0.02\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    const std::string module_tape="const 0.6\nconst 0.4\nconst 0\nconst 1\nconst -0.04\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=atlas.atlas_h=640;atlas.tri_order={0,1};atlas.charts.resize(1);
    auto& chart=atlas.charts[0];chart={};chart.tangent[0]=chart.bitangent[1]=1;
    chart.rect_w=chart.rect_h=640;chart.texels_per_meter=128;chart.tri_count=2;
    const float positions[]={0,0,0,4,0,0,4,4,0,0,4,0};
    const float normals[]={0,0,1,0,0,1,0,0,1,0,0,1};
    const float uv[]={4.f/640,4.f/640,516.f/640,4.f/640,516.f/640,516.f/640,4.f/640,516.f/640};
    const uint32_t indices[]={0,1,2,0,2,3},carrier=1;const uint8_t weights[]={255,255,255,255};
    uint32_t slots[2]{};
    for(int i=0;i<2;++i) {
        vt::VtPartContext ctx;ctx.variant_hash=hashes[i];ctx.rung_count=1;ctx.atlas=&atlas;
        ctx.positions=positions;ctx.normals=normals;ctx.surface_uvs=uv;ctx.indices=indices;
        ctx.vertex_count=4;ctx.triangle_count=2;ctx.dominant_material=1;
        ctx.surface_weights=weights;ctx.surface_materials=&carrier;ctx.surface_material_count=1;
        ctx.surface_tape_text=fallback.c_str();ctx.surface_tape_hash=hashes[i];
        slots[i]=residency.register_variant(hashes[i],0,atlas,ctx);
        if(!slots[i]){error="coverage fixture registration failed";return false;}
    }
    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    vt::VtPeriodicDomain domain;std::shared_ptr<const vt::VtPartSnapshot> module_input;
    if(!vt::vt_make_periodic_domain(frame,{1,1},256,domain,error) ||
       !vt::vt_make_periodic_material(domain,{},module_tape,1,module_input,error))return false;
    vt::VtMaterialModuleLease module;
    if(!residency.acquire_material_module(module_input,module,error))return false;
    vt::VtReceiverMaterialChart mapping;mapping.module=module;mapping.frame=frame;
    for(auto hash:hashes)if(!residency.bind_receiver_materials(hash,0,{mapping},error))return false;
    if(!vt_prepare_tests::until([&]{return advance() && residency.slot_active(slots[0]) &&
        residency.slot_active(slots[1]) && residency.material_module_binding(module).slot;}))return false;
    if(!advance())return false; // publish both ready module mappings
    const auto binding=residency.material_module_binding(module);
    vt_module_residency_tests::Sampler sampler;if(!sampler.init(vk,residency,error))return false;
    const auto probe_at=[&](int i,float x=1.5f,float y=1.5f) {
        vt_material_domain_tests::Probe p{};p.slots[0]=slots[i];p.slots[3]=0x80000000u;
        p.uv[0]=(4+x*128)/640;p.uv[1]=(4+y*128)/640;p.uv[2]=0;
        p.derivatives[2]=1;p.derivatives[3]=1.f/128;return p;
    };
    const auto page=[&](int i,uint32_t x=1,uint32_t y=1) {
        return residency.resident_page_slot_for_test(slots[i],{0,x,y});
    };
    const auto load=[&](int i,uint32_t x=1,uint32_t y=1) {
        const vt::VtFeedbackRequest request{slots[i]-1,0,x,y};
        residency.inject_feedback_for_test(&request,1);
        return vt_prepare_tests::until([&]{return advance() && page(i,x,y)!=UINT32_MAX;});
    };
    const auto allocations=residency.stats().material_pages;
    const auto coverage_fills=compositor->stats().coverage_pages_filled;
    if(!load(0))return false;
    CHECK(residency.coverage_only_page_for_test(page(0)) && residency.stats().coverage_only_pages==1 &&
          residency.stats().material_pages==allocations && compositor->stats().coverage_pages_filled>coverage_fills,
          "coverage-only: fine interior fills AUX/geometry without allocating or encoding private material");
    if(!load(1) || !load(0,0,1))return false;
    CHECK(!residency.coverage_only_page_for_test(page(1)) && !residency.coverage_only_page_for_test(page(0,0,1)),
          "coverage-only: legacy producer and chart boundaries keep complete material");
    vt::VtVariantLayout layout;vt::vt_build_layout(640,640,layout);
    CHECK(!residency.coverage_only_page_for_test(residency.resident_page_slot_for_test(slots[0],{layout.mip_count-1,0,0})),
          "coverage-only: mandatory receiver tail is always a complete finite fallback");
    vt_material_domain_tests::Result crossing[2]{};
    for(int i=0;i<2;++i) {
        auto p=probe_at(i);p.uv[0]=127.f/640;
        p.derivatives[0]=(128.f/640)*std::sqrt(1.f-.25f*.25f);p.derivatives[2]=.25f;
        if(!sampler.sample_probe(vk,residency,frames,serial,p,crossing[i],error))return false;
    }
    CHECK(crossing[0].pom[1]==1 && crossing[1].pom[1]==1 &&
          std::abs(crossing[0].pom[0]-crossing[1].pom[0])<1e-6f,
          "coverage-only: grazing POM crosses from full boundary pages to coverage interiors without a false geometry mismatch");
    auto finite_probe=probe_at(0);finite_probe.slots[3]=0x40000000u;
    vt_material_domain_tests::Result finite{};
    if(!sampler.sample_probe(vk,residency,frames,serial,finite_probe,finite,error))return false;
    CHECK(finite.material_request[0]==0 && finite.metrics[1]==float(layout.mip_count-1) &&
          finite.slots[0]!=page(0) && finite.receiver_request[3]==0 &&
          std::abs(finite.channels[0][0]-.1f)<.01f && std::abs(finite.metrics[0]+.02f)<2e-6f,
          "coverage-only: safety fallback uses complete tail coverage/material while preserving fine demand");
    for(float x:{1.05f,1.5f,1.93f})for(float y:{1.05f,1.5f,1.93f}) {
        vt_material_domain_tests::Result covered{},full{};
        if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0,x,y),covered,error) ||
           !sampler.sample_probe(vk,residency,frames,serial,probe_at(1,x,y),full,error))return false;
        CHECK(covered.material_request[0]==binding.slot && covered.pom[1]==1,
              "coverage-only: production sampler and POM resolve the retained module");
        CHECK(std::memcmp(covered.channels,full.channels,sizeof(covered.channels))==0 &&
              std::abs(covered.pom[0]-full.pom[0])<1e-6f,
              "coverage-only: PBR, height and AUX match a fully generated receiver page");
    }
    auto bounded=mapping;bounded.u_range_m={2.5f,4};writer->refuse_owner=hashes[0];
    if(!residency.bind_receiver_materials(hashes[0],0,{bounded},error))return false;
    for(int i=0;i<3;++i) {
        vt_material_domain_tests::Result result{};
        if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0),result,error))return false;
        CHECK(result.material_request[0]==binding.slot && residency.coverage_only_page_for_test(page(0)) &&
              std::abs(result.channels[0][0]-.6f)<.01f,
              "coverage-only: failed finite refill preserves the old table and visible module");
    }
    CHECK(writer->refusals>=3,"coverage-only: fixture actually refused completed finite replacements");
    writer->refuse_owner=0;
    if(!vt_prepare_tests::until([&]{return advance() && !residency.coverage_only_page_for_test(page(0));}))return false;
    vt_material_domain_tests::Result result{};
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0),result,error))return false;
    CHECK(result.material_request[0]==0 && std::abs(result.channels[0][0]-.1f)<.01f &&
          std::abs(result.metrics[0]+.02f)<2e-6f && residency.stats().coverage_only_pages==0,
          "coverage-only: narrowed mapping publishes only after complete finite pixels return");
    if(!residency.bind_receiver_materials(hashes[0],0,{mapping},error) || !advance())return false;
    residency.invalidate_all_content();
    if(!vt_prepare_tests::until([&]{return advance() && residency.coverage_only_page_for_test(page(0));}))return false;
    writer->refuse_owner=hashes[0];
    if(!residency.bind_receiver_materials(hashes[0],0,{},error))return false;
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0),result,error))return false;
    CHECK(result.material_request[0]==binding.slot,"coverage-only: removal also retains old module until refill succeeds");
    writer->refuse_owner=0;
    if(!vt_prepare_tests::until([&]{return advance() && !residency.coverage_only_page_for_test(page(0));}) ||
       !sampler.sample_probe(vk,residency,frames,serial,probe_at(0),result,error))return false;
    CHECK(result.material_request[0]==0 && std::abs(result.channels[0][0]-.1f)<.01f &&
          residency.stats().dirty_pages==0,"coverage-only: mapping removal restores finite material and drains retries");
    // Recreate a coverage page, then release every external module lease.
    if(!residency.bind_receiver_materials(hashes[0],0,{mapping},error) || !advance())return false;
    residency.invalidate_all_content();
    if(!vt_prepare_tests::until([&]{return advance() && residency.coverage_only_page_for_test(page(0));}))return false;
    module.reset();mapping.module.reset();bounded.module.reset();
    for(auto hash:hashes)residency.release_variant(hash);
    CHECK(residency.stats().coverage_only_pages==0 && residency.stats().module_variants==1,
          "coverage-only: released receivers retire coverage ownership while retaining the reader's module");
    for(uint32_t i=0;i<vt::kVtRetireHorizonFrames+1;++i)if(!advance())return false;
    CHECK(residency.stats().module_variants==0,"coverage-only: retired mapping eventually releases the module");
    std::printf("VT_RECEIVER_COVERAGE parity_samples=9 refused_replacements=%u transition=narrow_remove_release\n",writer->refusals);
    return true;
}

inline bool run_impl(matter::VulkanDevice& vk,std::string& error) {
    vt_queue_tests::Budgets budgets;
    vt::VtResidency residency;vt_queue_tests::Frames frames(vk);uint64_t serial=0;
    if (!frames.valid() || !residency.init(vk,error)) return false;
    auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);
    if(!producer)return false;
    vt::VtCompositorMaterial materials[2]{};producer->set_materials(materials,2);
    residency.set_filler(std::move(producer));
    const auto advance=[&]{return frames.next(residency,++serial);};
    constexpr uint64_t hashes[]={0xfeed101,0xfeed102,0xfeed104};
    const gpu_meshing::FaceFrame module_frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    const gpu_meshing::FaceFrame receiver_frames[]={
        {{0,0,0},{1,0,0},{0,1,0},{0,0,1}},
        {{2,3,4},{0,1,0},{0,0,1},{1,0,0}},
        {{-3,2,-1},{-1,0,0},{0,1,0},{0,0,-1}}};
    const matter::Float3 tangent[]={{1,0,0},{0,0,1},{1,0,0}};
    const matter::Float3 bitangent[]={{0,1,0},{0,-1,0},{0,-1,0}};
    uint32_t slots[3]{};std::shared_ptr<const vt::VtPartSnapshot> receivers[3];
    const std::string fallback="const 0.1\nconst 0.7\nconst 0\nconst 1\nconst -0.02\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    const uint32_t indices[]={0,1,2,0,2,3},carrier=1;const uint8_t weights[]={255,255,255,255};
    for(int i=0;i<3;++i) {
        const float width=float(1u<<i),height=2,tpm=32;
        const auto& f=receiver_frames[i];const auto t=tangent[i],b=bitangent[i];
        chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=(uint32_t(width*tpm)+8+127)/128*128;
        atlas.atlas_h=128;atlas.tri_order={0,1};atlas.charts.resize(1);
        auto& c=atlas.charts[0];c.origin[0]=f.origin_m.x;c.origin[1]=f.origin_m.y;c.origin[2]=f.origin_m.z;
        // Chart projection is unchanged by an origin offset normal to its plane.
        c.origin[0]+=f.n.x*.37f;c.origin[1]+=f.n.y*.37f;c.origin[2]+=f.n.z*.37f;
        c.tangent[0]=t.x;c.tangent[1]=t.y;c.tangent[2]=t.z;
        c.bitangent[0]=b.x;c.bitangent[1]=b.y;c.bitangent[2]=b.z;
        c.rect_w=atlas.atlas_w;c.rect_h=atlas.atlas_h;c.texels_per_meter=tpm;c.tri_count=2;
        float positions[12],normals[12],uv[8];
        for(int j=0;j<4;++j) {
            const float x=(j==1||j==2)?width:0,y=j>=2?height:0;
            positions[j*3]=f.origin_m.x+t.x*x+b.x*y;positions[j*3+1]=f.origin_m.y+t.y*x+b.y*y;
            positions[j*3+2]=f.origin_m.z+t.z*x+b.z*y;
            normals[j*3]=f.n.x;normals[j*3+1]=f.n.y;normals[j*3+2]=f.n.z;
            uv[j*2]=(4+x*tpm)/atlas.atlas_w;uv[j*2+1]=(4+y*tpm)/atlas.atlas_h;
        }
        vt::VtPartContext ctx;ctx.variant_hash=hashes[i];ctx.rung_count=1;ctx.atlas=&atlas;
        ctx.positions=positions;ctx.normals=normals;ctx.surface_uvs=uv;ctx.indices=indices;
        ctx.vertex_count=4;ctx.triangle_count=2;ctx.dominant_material=1;
        ctx.surface_weights=weights;ctx.surface_materials=&carrier;ctx.surface_material_count=1;
        ctx.surface_tape_text=fallback.c_str();ctx.surface_tape_hash=hashes[i];
        receivers[i]=vt::VtPartSnapshot::capture(atlas,ctx);
        slots[i]=residency.register_variant(hashes[i],0,atlas,ctx);
        if(!slots[i]){error="receiver mapping fixture registration failed";return false;}
    }
    if(!vt_prepare_tests::until([&]{return advance() && residency.slot_active(slots[0]) &&
        residency.slot_active(slots[1]) && residency.slot_active(slots[2]);}))return false;
    vt_module_residency_tests::Sampler sampler;if(!sampler.init(vk,residency,error))return false;
    vt::VtPeriodicDomain domain;
    if(!vt::vt_make_periodic_domain(module_frame,{1,1},256,domain,error))return false;
    auto stamp=std::make_shared<surface_stamp::Stamp>(*vt_finite_test::source(false,64));
    stamp->domain[0]=stamp->domain[1]=0;stamp->domain[2]=stamp->domain[3]=1;
    stamp->height_min_m=-.045f;stamp->height_max_m=-.035f;stamp->content_digest+=0x90000000;
    for(const auto& level:stamp->levels)for(uint32_t y=0;y<level.height;++y)for(uint32_t x=0;x<level.width;++x) {
        auto& p=stamp->pixels[level.offset+size_t(y)*level.width+x];
        const float a=6.283185307f*(x+.5f)/level.width,b=6.283185307f*(y+.5f)/level.height;
        p.albedo_coverage[0]=.4f+.2f*std::sin(a);p.albedo_coverage[1]=.4f+.2f*std::cos(b);
        p.orm_height[3]=-.04f+.005f*std::cos(a);
        p.normal_detail[0]=.3f;p.normal_detail[1]=.4f;p.normal_detail[2]=std::sqrt(.75f);
    }
    vt::VtFiniteSourceBinding source;source.stamp=stamp;source.frame=module_frame;
    std::shared_ptr<const vt::VtPartSnapshot> module_input;
    const std::string base="const 0.2\nconst 0.7\nconst 0\nconst 1\nconst -0.08\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.08 0\n";
    if(!vt::vt_make_periodic_material(domain,{source},base,1,module_input,error))return false;
    vt::VtMaterialModuleLease module;
    if(!residency.acquire_material_module(module_input,module,error))return false;
    std::vector<vt::VtReceiverMaterialChart> mappings(3);
    for(int i=0;i<3;++i) {
        auto& m=mappings[i];m.module=module;m.frame=receiver_frames[i];m.phase={.13f,.21f};m.datum_m=-.005f*i;
        if(!residency.bind_receiver_materials(hashes[i],0,{m},error))return false;
    }
    const auto probe_at=[&](int i,float x,float y) {
        vt_material_domain_tests::Probe p{};
        p.slots[0]=slots[i];p.slots[3]=0x80000000u;
        p.uv[0]=(4+x*32)/receivers[i]->geometry->atlas.atlas_w;p.uv[1]=(4+y*32)/128;p.uv[2]=-8;
        p.derivatives[2]=1;p.derivatives[3]=1.f/32;
        return p;
    };
    vt_material_domain_tests::Result pending{};
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0,.5,.7),pending,error))return false;
    CHECK(pending.material_request[0]==0 && std::abs(pending.channels[0][0]-.1f)<.01f,
        "receiver: pending module tail preserves complete finite material");
    if(!vt_prepare_tests::until([&]{return advance() && residency.material_module_binding(module).slot;}))return false;
    const auto binding=residency.material_module_binding(module);
    float maximum_error=0;uint32_t comparisons=0;
    for(int i=0;i<3;++i)for(float x:{.35f,.65f}) {
        const float y=.7f;
        auto probe=probe_at(i,x,y);
        vt_material_domain_tests::Result mapped{},expected{};
        if(!sampler.sample_probe(vk,residency,frames,serial,probe,mapped,error))return false;
        CHECK(mapped.material_request[0]==binding.slot && mapped.material_request[3]==0,
            "receiver: fine material demand survives a sub-texel receiver footprint");
        vt::VtFeedbackRequest demand{binding.slot-1,0,mapped.material_request[1],mapped.material_request[2]};
        residency.inject_feedback_for_test(&demand,1);
        if(!vt_prepare_tests::until([&]{return advance() && residency.resident_page_slot_for_test(binding.slot,
            {0,demand.px,demand.py})!=UINT32_MAX;}))return false;
        if(!sampler.sample_probe(vk,residency,frames,serial,probe,mapped,error))return false;
        auto raw=probe;raw.slots[1]=binding.slot;raw.slots[2]=binding.generation;raw.slots[3]=0;
        // Independent physical-coordinate oracle, deliberately not the CPU
        // atlas-affine mapping helper under test.
        const auto t=tangent[i],b=bitangent[i];const auto& f=receiver_frames[i];
        const float px=t.x*x+b.x*y,py=t.y*x+b.y*y,pz=t.z*x+b.z*y;
        raw.uv[2]=px*f.u.x+py*f.u.y+pz*f.u.z+.13f;
        raw.uv[3]=px*f.v.x+py*f.v.y+pz*f.v.z+.21f;
        std::fill(raw.derivatives,raw.derivatives+4,0.f);
        if(!sampler.sample_probe(vk,residency,frames,serial,raw,expected,error))return false;
        for(int channel:{0,2,4})for(int c=0;c<3;++c)
            maximum_error=std::max(maximum_error,std::abs(mapped.channels[channel][c]-expected.channels[channel][c]));
        const float nx=expected.channels[1][0]*2-1,ny=expected.channels[1][1]*2-1;
        const float normal_x=i==0?nx:i==1?ny:-nx,normal_y=i==0?ny:i==1?-nx:-ny;
        CHECK(std::abs(mapped.channels[1][0]*2-1-normal_x)<.001f &&
              std::abs(mapped.channels[1][1]*2-1-normal_y)<.001f,"receiver: normal frame rotates with the physical face");
        CHECK(std::abs(mapped.metrics[0]-expected.metrics[0]-mappings[i].datum_m)<2e-6f,
            "receiver: signed height datum stays separate from texture decode");
        CHECK(mapped.pom[1]==1 && std::abs(mapped.pom[0]+mapped.metrics[0])<2e-5f,
            "receiver: production POM uses module height despite coarse receiver coverage");
        CHECK(mapped.receiver_request[0]==slots[i] && mapped.metrics[1]>mapped.metrics[2],
            "receiver: independent coverage and material residency");
        CHECK((mapped.visible_feedback[0]>>16)==binding.slot && (mapped.visible_feedback[0]&65535)==slots[i],
            "receiver: automatically mapped samples emit both feedback owners");
        ++comparisons;
    }
    CHECK(maximum_error<.001f,"receiver: mapped PBR matches independent physical module coordinates");
    // Rejection must leave the last published mapping intact.
    auto invalid=mappings[0];invalid.frame.n={0,1,0};
    CHECK(!residency.bind_receiver_materials(hashes[0],0,{invalid},error),"receiver: reject a non-rigid/wrong-plane mapping");
    error.clear();
    invalid=mappings[0];invalid.frame.origin_m.z+=.01f;
    CHECK(!residency.bind_receiver_materials(hashes[0],0,{invalid},error),"receiver: reject a rigid projection plane displaced from the mesh");
    error.clear();
    vt_material_domain_tests::Result retained{};
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0,.5,.7),retained,error))return false;
    CHECK(retained.material_request[0]==binding.slot,"receiver: rejected mapping preserves published module");
    auto bounded=mappings[0];bounded.u_range_m={.6f,.9f};
    if(!residency.bind_receiver_materials(hashes[0],0,{bounded},error))return false;
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0,.35f,.7f),retained,error))return false;
    CHECK(retained.material_request[0]==0 && std::abs(retained.channels[0][0]-.1f)<.01f &&
        retained.pom[1]==1 && std::abs(retained.pom[0]-pending.pom[0])<2e-5f &&
        std::abs(retained.metrics[0]+.02f)<2e-6f,
        "receiver: finite end interval preserves fallback height and its existing coarse-coverage POM fade");
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0,.65f,.7f),retained,error))return false;
    CHECK(retained.material_request[0]==binding.slot,"receiver: interval interior retains shared material");
    bounded.module.reset();
    // Drop every external lease: pages and their immutable mapping must own it.
    module.reset();for(auto& m:mappings)m.module.reset();invalid.module.reset();
    CHECK(residency.stats().module_variants==1,"receiver: three differently sized/oriented receivers own one module");
    if(!residency.bind_receiver_materials(hashes[0],0,{},error))return false;
    if(!sampler.sample_probe(vk,residency,frames,serial,probe_at(0,.5,.7),retained,error))return false;
    CHECK(retained.material_request[0]==0 && std::abs(retained.channels[0][0]-.1f)<.01f,
        "receiver: removing mapping restores all finite fallback channels without rebaking");
    for(auto hash:hashes)residency.release_variant(hash);
    CHECK(residency.stats().module_variants==1,"receiver: retired pages retain their module through the reader horizon");
    for(uint32_t i=0;i<vt::kVtRetireHorizonFrames+1;++i)if(!advance())return false;
    CHECK(residency.stats().module_variants==0,"receiver: final retired mapping releases its module");
    std::printf("VT_RECEIVER_MATERIAL receivers=3 comparisons=%u max_channel_error=%.8f mapping=PBR_height_normal_feedback retirement=checked\n",comparisons,maximum_error);
    return true;
}
inline void run(matter::VulkanDevice& vk) {
    std::string error;CHECK(run_impl(vk,error),error.empty()?"receiver material native integration":error.c_str());
    error.clear();CHECK(run_coverage_impl(vk,error),error.empty()?"coverage-only receiver integration":error.c_str());
    error.clear();CHECK(run_occlusion_impl(vk,error),error.empty()?"receiver occlusion integration":error.c_str());
    CHECK(vk.validation_error_count()==0,"receiver material has zero Vulkan validation errors");
}
} // namespace vt_receiver_material_tests
