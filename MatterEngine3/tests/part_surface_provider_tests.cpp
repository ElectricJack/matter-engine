#include "check.h"
#include "provider/local_provider.h"
#include "part_surface.h"
#include "render/vt_snapshot.h"
#include "render/part_store.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
namespace fs=std::filesystem;
namespace {
void write(const fs::path& p,const std::string& s) {fs::create_directories(p.parent_path());std::ofstream(p)<<s;}
void test_authored_density(const fs::path& root) {
    script_host::ScriptHost host;
    script_host::BakeOptions options;
    options.parts_dir=(root/"density").string();options.retain_geometry=true;
    fs::create_directories(fs::path(options.parts_dir)/"parts");
    const auto source=[](const std::string& declaration){
        return "class Density extends Part {static lodBudgets=[1];static noImpostor=true;"
            "static params={density:128};"+declaration+R"(
            build(){this.fill(8);this.beginShape(0);
            this.vertex(0,0,0);this.vertex(1,0,0);this.vertex(0,1,0);this.endShape();}}
        )";
    };
    const auto authored=source("static vtTexelsPerMeter(p){return p.density;}");
    const auto baked=host.bake_source(authored,"{\"density\":256}",options);
    CHECK(baked.error.ok && baked.geometry,"authored density bakes with retained geometry");
    if(!baked.geometry)return;
    CHECK(baked.geometry->render_policy.vt_texels_per_meter==256,"density method sees merged params");
    viewer::PartStore store(options.parts_dir);
    auto disk=store.stage_load(baked.resolved_hash);
    auto memory=store.stage_from_bake(baked.resolved_hash,*baked.geometry);
    const auto chart_density=[](const auto& staged){
        return staged.ok && !staged.lp.lod_charts.empty() && !staged.lp.lod_charts[0].charts.empty()
            ? staged.lp.lod_charts[0].charts[0].texels_per_meter : 0.f;
    };
    std::string difference;
    CHECK(chart_density(disk)==256 && viewer::staged_parts_equal(disk,memory,&difference),
          "disk and retained bake produce identical authored-density charts");
    memory.lp.render_policy.vt_texels_per_meter=128;
    CHECK(!viewer::staged_parts_equal(disk,memory,&difference) && difference=="render_policy.vt_texels_per_meter",
          "snapshot comparison detects changed density metadata");
    memory.lp.render_policy=disk.lp.render_policy;
    memory.lp.render_policy.shared_surfaces=true;
    CHECK(!viewer::staged_parts_equal(disk,memory,&difference) && difference=="render_policy.shared_surfaces",
          "snapshot comparison detects changed shared-surface policy");
    auto terrain=store.stage_load(baked.resolved_hash,0,true,{},64);
    CHECK(chart_density(terrain)==64,"streamed terrain retains world density authority");
    const auto defaults=host.bake_source(authored,"{}",options);
    CHECK(defaults.error.ok && defaults.geometry && defaults.geometry->render_policy.vt_texels_per_meter==128 &&
          defaults.resolved_hash!=baked.resolved_hash,"density edits select a new cached part identity");
    options.output_mode=script_host::BakeOptions::OutputMode::RuntimeLeafMemory;
    options.parts_dir=(root/"density-memory-only").string();
    const auto prepared=host.bake_source(authored,"{\"density\":256}",options);
    CHECK(prepared.error.ok && prepared.geometry && !fs::exists(options.parts_dir),
          "memory-only preparation carries density without publishing files");
    if(prepared.geometry) {
        const auto staged=store.stage_from_bake(prepared.resolved_hash,*prepared.geometry);
        CHECK(viewer::staged_parts_equal(disk,staged,&difference),"runtime-only and durable density match");
    }
    options.output_mode=script_host::BakeOptions::OutputMode::Persistent;
    options.time_budget_ms=50;
    for(const char* declaration:{"static vtTexelsPerMeter=0;","static vtTexelsPerMeter=-1;",
        "static vtTexelsPerMeter=.5;","static vtTexelsPerMeter=2049;","static vtTexelsPerMeter=NaN;",
        "static vtTexelsPerMeter=Infinity;","static vtTexelsPerMeter='128';",
        "static vtTexelsPerMeter=true;","static vtTexelsPerMeter=null;",
        "static vtTexelsPerMeter(){return undefined;}",
        "static get vtTexelsPerMeter(){throw new Error('density getter');}",
        "static vtTexelsPerMeter(){while(true){}}"}) {
        const auto invalid=host.bake_source(source(declaration),"{}",options);
        CHECK(!invalid.error.ok && !invalid.geometry && invalid.written_path.empty() &&
              !fs::exists(options.parts_dir),"invalid or unbounded density rejected before artifact publication");
    }
    options.time_budget_ms=0;
    fs::create_directories(fs::path(options.parts_dir)/"parts");
    const auto numeric=host.bake_source(source("static vtTexelsPerMeter=64;"),"{}",options);
    CHECK(numeric.error.ok && numeric.geometry && numeric.geometry->render_policy.vt_texels_per_meter==64,
          "plain numeric density declaration is supported");
    std::printf("PART_VT_DENSITY merged_params=checked disk_memory=equal invalid_metadata=rejected\n");
}
void run() {
    const auto root=fs::temp_directory_path()/("matter-part-surface-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{root};
    test_authored_density(root);
    write(root/"objects/Brick.js",R"(
class Brick extends Part {
 static lodBudgets=[1]; static noImpostor=true; static params={tone:.3};
 static finiteSurface(p){return {version:1,material:8,pixelM:.01,
  boundsMinM:[-.04,-.04,-.04],boundsMaxM:[.04,.04,.04],
  solid:{version:1,voxelM:.01,ops:[{shape:'box',halfExtentsM:[.04,.04,.04]}]},
  appearance:s=>({baseColor:[p.tone,.1,.05],roughness:.8,height:0,heightRange:[0,0]}),
  base:s=>({baseColor:[.1,.1,.1],roughness:1,height:0,heightRange:[0,0]})};}
 build(){this.fill(8);this.beginShape(0);
  this.surfaceVertex(-.04,-.04,.04,0,0,1,0,0);
  this.surfaceVertex(.04,-.04,.04,0,0,1,.08,0);
  this.surfaceVertex(-.04,.04,.04,0,0,1,0,.08);this.endShape();}
})");
    write(root/"worlds/Surface.js","class Surface extends World {static roots=[{module:'Brick',params:{}}];}");
    auto cfg=viewer::LocalProviderConfig::for_project(root.string(),"Surface","");
    unsigned projected=0,shaded=0,published=0,queued=0;
    unsigned fail_shade=0; bool discard=false;
    std::function<bool(std::string&)> abandoned;
    std::shared_ptr<const part_surface::Prepared> active;
    cfg.gpu_run=[&](const auto&,std::function<bool(std::string&)> fn,std::string& error){
        ++queued;
        if(discard){abandoned=std::move(fn);error="cancelled";return false;}
        return fn(error);
    };
    cfg.vk_solid_face_project=[&](const auto& j,auto& p,auto& s,auto& e,const auto& c){
        ++projected;return gpu_meshing::project_solid_face_reference(j,p,s,e,c);
    };
    cfg.vk_face_material_bake=[&](const auto& j,auto& p,auto&,auto& e,const auto& c){
        if(++shaded==fail_shade){e={gpu_meshing::ErrorCode::Cancelled,"injected material cancellation"};return false;}
        return gpu_meshing::bake_face_material_reference(j,p,e,c);
    };
    cfg.publish_part_surface=[&](auto candidate,std::string&){++published;active=std::move(candidate);return true;};
    std::string error;viewer::WorldManifest world;
    {
        viewer::LocalProvider provider(cfg);
        CHECK(provider.connect(world,error),error.c_str());
        CHECK(projected==6 && shaded==6 && published==1 && queued==13,"cold source prepares six complete faces and publishes once");
        CHECK(active && active->sources->bindings.size()==6,"provider publishes a complete catalog");
        if (!active) return;
        CHECK(provider.ensure_part_baked(active->part_hash,error),error.c_str());
        CHECK(projected==6 && shaded==6 && published==1,"same provider reuses its immutable prepared source");
    }
    if (!active) return;
    const auto first=active;
    projected=shaded=published=queued=0;
    {
        viewer::LocalProvider provider(cfg);
        CHECK(provider.connect(world,error),error.c_str());
        CHECK(projected==0 && shaded==6 && published==1,"warm geometry cache hit still prepares/publishes material on cache load");
        CHECK(active->sources->content_hash==first->sources->content_hash,"warm source is deterministic");
    }
    cfg.root_params_json="{\"tone\":0.6}";
    projected=shaded=published=queued=0;
    {
        viewer::LocalProvider provider(cfg);CHECK(provider.connect(world,error),error.c_str());
        CHECK(projected==0 && shaded==6 && published==1 && active->part_hash!=first->part_hash &&
              active->sources->content_hash!=first->sources->content_hash,"appearance edit reuses geometry and publishes a new immutable material");
    }
    const auto previous=active;
    projected=shaded=published=0;fail_shade=3;
    {
        viewer::LocalProvider provider(cfg);CHECK(!provider.connect(world,error),"injected partial material failure is reported");
        CHECK(published==0 && active==previous,"partial material set never replaces prior source");
    }
    fail_shade=0;discard=true;projected=shaded=published=0;
    {viewer::LocalProvider provider(cfg);CHECK(!provider.connect(world,error),"discarded GPU job fails preparation");}
    CHECK(abandoned!=nullptr && shaded==0 && published==0,"discarded envelope retains its owned inputs");
    if(abandoned){CHECK(abandoned(error),"discarded material envelope can finish safely after provider destruction");abandoned={};}
    CHECK(active==previous,"late abandoned callback cannot publish source");
    float positions[]={-.04f,-.04f,.04f, .04f,-.04f,.04f, -.04f,.04f,.04f};
    float normals[]={0,0,1, 0,0,1, 0,0,1};uint32_t indices[]={0,1,2};
    vt::VtPartContext ctx;ctx.positions=positions;ctx.normals=normals;ctx.vertex_count=3;
    ctx.indices=indices;ctx.triangle_count=1;
    part_surface::BindingScratch scratch;
    CHECK(part_surface::bind(*active,ctx,scratch,error),error.c_str());
    CHECK(scratch.ids==std::vector<uint32_t>({1,1,1}) && ctx.surface_material_count==1 &&
          ctx.surface_weights[0]==255 && ctx.finite_sources==active->sources,"receiver binding selects its outward +Z source");
    positions[2]=.05f;
    CHECK(!part_surface::bind(*active,ctx,scratch,error),"off-plane receiver rejected");
    CHECK(scratch.ids==std::vector<uint32_t>({1,1,1}),"failed binding leaves prior owned scratch intact");
    // Two separated physical cubes share one source and one receiver face.
    script_host::ScriptHost host;script_host::BakeError eval_error;
    script_host::EvaluatedFiniteSurface composite;
    CHECK(host.evaluate_finite_surface(R"(
class Composite extends Part {static finiteSurface(){
 const base=()=>({baseColor:[.1,.1,.1],roughness:1,height:-.01,heightRange:[-.01,-.01]});
 const source={version:1,material:8,pixelM:.01,boundsMinM:[-.04,-.04,-.04],boundsMaxM:[.04,.04,.04],
 solid:{version:1,voxelM:.01,ops:[{shape:'box',halfExtentsM:[.04,.04,.04]}]},
 appearance:()=>({baseColor:[.3,.1,.05],roughness:.8,height:0,heightRange:[0,0]}),base};
 return {version:2,material:8,pixelM:.01,boundsMinM:[-.09,-.04,-.04],boundsMaxM:[.09,.04,.04],base,
 sources:[source,source],placements:[{id:'left',source:0,matrix:[1,0,0,-.05,0,1,0,0,0,0,1,0,0,0,0,1]},
 {id:'right',source:1,matrix:[1,0,0,.05,0,1,0,0,0,0,1,0,0,0,0,1]}]};}}
)","{}",composite,eval_error),eval_error.message.c_str());
    part_surface::SourceCache cache;part_surface::Stats stats;gpu_meshing::Error preparation_error;
    std::shared_ptr<const part_surface::Prepared> combined;
    projected=shaded=0;
    CHECK(part_surface::prepare(composite,root.string(),cfg.vk_solid_face_project,cfg.vk_face_material_bake,
          combined,stats,preparation_error,{},&cache),preparation_error.message.c_str());
    CHECK(combined && combined->sources->receivers.size()==6 && combined->sources->bindings.size()==10,
          "two sources produce ten exposed placements across six receiver faces");
    CHECK(shaded==6 && stats.material_hits==6,"duplicate source material is prepared once");
    positions[0]=-.09f;positions[2]=.04f;positions[3]=.09f;positions[6]=-.09f;
    if(combined) {
        CHECK(part_surface::bind(*combined,ctx,scratch,error),error.c_str());
        CHECK(scratch.ids==std::vector<uint32_t>({1,1,1}),"wall triangle may span multiple bricks and mortar");
        CHECK(!combined->sources->lookup.empty(),"composite publishes a spatial candidate index");
        const auto old_hash=combined->sources->content_hash;
        std::shared_ptr<const part_surface::Prepared> warm;
        CHECK(part_surface::prepare(composite,root.string(),cfg.vk_solid_face_project,cfg.vk_face_material_bake,
              warm,stats,preparation_error,{},&cache),preparation_error.message.c_str());
        CHECK(warm && warm->sources->content_hash==old_hash && shaded==6 && stats.material_hits==12,
              "complete receiver reuse preserves catalog and avoids all source shading");
    }
    // Module preparation reuses source pixels and stays independent of the
    // receiving wall's dimensions, identity and material phase.
    script_host::FiniteSurfaceModule module;
    module.frame.origin_m={-.1f,-.05f,.04f};
    module.frame.u={1,0,0};module.frame.v={0,1,0};module.frame.n={0,0,1};
    module.period_m={.2f,.1f};module.texels_per_m=100;
    module.placements=composite.placements;module.base_program=composite.base_program;
    script_host::FiniteSurfaceMaterialMapping mapping;
    mapping.frame=module.frame;mapping.u_range_m={.01f,.19f};
    composite.modules={module};composite.material_mappings={mapping};
    std::shared_ptr<const part_surface::Prepared> periodic;
    CHECK(part_surface::prepare(composite,root.string(),cfg.vk_solid_face_project,cfg.vk_face_material_bake,
          periodic,stats,preparation_error,{},&cache),preparation_error.message.c_str());
    CHECK(periodic && periodic->modules.size()==1 && periodic->material_mappings.size()==1 &&
          periodic->periodic_hash && shaded==6,"periodic preparation reuses all shaded source faces");
    if(periodic && !periodic->modules.empty()) {
        const auto pixels=periodic->modules[0];
        for(const auto& payload:pixels->context.finite_sources->payloads) {
            bool shared=false;
            for(const auto& finite:periodic->sources->payloads)if(finite==payload)shared=true;
            CHECK(shared,"wrapped placements retain existing source payloads without copying pixels");
        }
        auto resized=composite;resized.bounds_max_m[0]=.19f;resized.geometry.resolved_hash^=12345;
        resized.material_mappings[0].phase={.5f,.25f};resized.material_mappings[0].u_range_m[1]=.29f;
        std::shared_ptr<const part_surface::Prepared> larger;
        CHECK(part_surface::prepare(resized,root.string(),cfg.vk_solid_face_project,cfg.vk_face_material_bake,
              larger,stats,preparation_error,{},&cache),preparation_error.message.c_str());
        CHECK(larger && larger->modules[0]->context.variant_hash==pixels->context.variant_hash &&
              larger->periodic_hash!=periodic->periodic_hash && shaded==6,
              "receiver resize and phase change mapping identity without regenerating the shared material");
        auto invalid=composite;invalid.modules[0].frame.origin_m[2]+=.01f;
        const auto previous=periodic;
        CHECK(!part_surface::prepare(invalid,root.string(),cfg.vk_solid_face_project,cfg.vk_face_material_bake,
              periodic,stats,preparation_error,{},&cache) && periodic==previous,
              "a module with no matching source plane fails atomically and retains prior material");
    }
    std::printf("PART_SURFACE cold_faces=6 warm_geometry_dispatches=0 publication=atomic receiver_binding=passed\n");
    write(root/"objects/Stone.js",R"(
class Stone extends Part {
 static lodBudgets=[1];static noImpostor=true;static params={tone:.3};
 static vtTexelsPerMeter=256;
 static surface(p){return {version:1,material:8,recipe:s=>({
  baseColor:[s.x.mul(.01).add(p.tone),.1,.08],roughness:.9,
  height:s.noise3(31,3,1).mul(.001).sub(.002),heightRange:[-.003,-.001]})};}
 build(){this.fill(8);this.beginShape(0);
  this.surfaceVertex(-.04,-.04,.05,0,0,1,0,0);
  this.surfaceVertex(.04,-.04,.04,0,0,1,.08,0);
  this.surfaceVertex(-.04,.04,.06,0,0,1,0,.08);this.endShape();}
})");
    write(root/"worlds/Direct.js","class Direct extends World {static roots=[{module:'Stone',params:{}}];}");
    auto direct_cfg=viewer::LocalProviderConfig::for_project(root.string(),"Direct","");
    unsigned direct_publications=0;
    std::shared_ptr<const part_surface::Prepared> direct_active;
    direct_cfg.publish_part_surface=[&](auto value,std::string&){++direct_publications;direct_active=value;return true;};
    // No geometry projector, material-image baker or GPU queue is installed.
    for(unsigned pass=0;pass<2;++pass) {
        viewer::LocalProvider provider(direct_cfg);
        CHECK(provider.connect(world,error),error.c_str());
        CHECK(direct_publications==pass+1 && direct_active &&
              direct_active->kind==part_surface::Prepared::Kind::Direct && !direct_active->sources &&
              direct_active->modules.empty(),"cold/warm ordinary source publishes recipe without image preparation");
        if (!direct_active) continue;
        CHECK(provider.ensure_part_baked(direct_active->part_hash,error),error.c_str());
        CHECK(direct_publications==pass+1,"one immutable recipe per shared prototype");
        viewer::PartStore store(direct_cfg.cache_root);
        const auto* part=store.get_or_load(direct_active->part_hash);
        CHECK(part && part->render_policy.vt_texels_per_meter==256 &&
              !part->lod_charts.empty() && !part->lod_charts[0].charts.empty() &&
              part->lod_charts[0].charts[0].texels_per_meter==256,
              "cold and warm provider loads retain authored material density");
    }
    if(direct_active) {
        CHECK(part_surface::bind(*direct_active,ctx,scratch,error),error.c_str());
        CHECK(!ctx.finite_sources && !ctx.finite_source_ids && scratch.ids.empty() &&
              ctx.surface_tape_hash==direct_active->base_hash && ctx.surface_material_count==1 &&
              ctx.surface_weights[0]==255 && !ctx.surface_world_anchored,
              "ordinary source accepts off-plane geometry and clears old finite-source context");
        const auto original_direct=direct_active;
        direct_cfg.root_params_json="{\"tone\":0.6}";
        {viewer::LocalProvider provider(direct_cfg);CHECK(provider.connect(world,error),error.c_str());}
        CHECK(direct_active->part_hash!=original_direct->part_hash && direct_active->base_hash!=original_direct->base_hash,
              "direct source edits republish changed identities with cached geometry allowed");
        const auto previous_direct=direct_active;
        direct_cfg.publish_part_surface=[](auto,std::string& text){text="injected direct publish failure";return false;};
        {viewer::LocalProvider provider(direct_cfg);CHECK(!provider.connect(world,error),"direct publication failure propagates");}
        CHECK(direct_active==previous_direct,"failed direct publication retains prior immutable recipe");
    }
    std::printf("DIRECT_PART_SURFACE cold_warm=checked source_images=0 off_plane_binding=checked edits=checked\n");
    // Field-world assets are installed directly and are absent from ir_.bake_plan.
    // Mirror that production sequence; cached geometry must still publish a
    // material through the provider's owned service and GPU queue.
    write(root/"worlds/StreamingAssets.js",R"(
class StreamingAssets extends World {static roots=[];field(){}}
)");
    auto stream_cfg=viewer::LocalProviderConfig::for_project(root.string(),"StreamingAssets","");
    stream_cfg.cache_root=(root/"streaming-cache").string();
    unsigned stream_publications=0,stream_jobs=0;
    std::shared_ptr<const part_surface::Prepared> stream_material;
    stream_cfg.gpu_run=[&](const auto&,auto fn,std::string& text){++stream_jobs;return fn(text);};
    stream_cfg.publish_part_surface=[&](auto value,std::string&){++stream_publications;stream_material=value;return true;};
    std::ifstream stone_file(root/"objects/Stone.js");
    const std::string stone_source{std::istreambuf_iterator<char>(stone_file),{}};
    script_host::ScriptHost asset_host;
    part_graph::BakeInputs inputs;inputs.module="Stone";inputs.source=stone_source;
    inputs.params=part_graph::params_from_json("{\"tone\":0.4}");
    const auto asset_hash=asset_host.resolve_hash(stone_source,"{\"tone\":0.4}",nullptr,0);
    for(unsigned pass=0;pass<2;++pass) {
        viewer::LocalProvider provider(stream_cfg);
        CHECK(provider.install_graph(error),error.c_str());
        CHECK(!provider.ensure_part_baked(asset_hash,error),"streamed asset is outside the ordinary graph bake plan");
        auto& baker=provider.host_baker();
        CHECK(baker.cached(asset_hash)==(pass==1),"stream asset fixture exercises cold and warm geometry");
        if(!baker.cached(asset_hash)) {
            baker.set_baking_module(inputs.module);
            CHECK(baker.bake(inputs.source,inputs.params,{}, {}, {},asset_hash),"stream asset direct geometry bake");
        }
        CHECK(provider.ensure_part_surface(asset_hash,inputs,error),error.c_str());
        CHECK(stream_material && stream_material->part_hash==asset_hash && stream_publications==pass+1 &&
              stream_jobs==pass+1,"external asset publishes one owned material on cold and warm installs");
        CHECK(provider.ensure_part_surface(asset_hash,inputs,error) && stream_publications==pass+1,
              "repeated streamed references share the prepared material");
    }
    stream_cfg.publish_part_surface=[](auto,std::string& text){text="injected stream material failure";return false;};
    {
        viewer::LocalProvider provider(stream_cfg);
        CHECK(provider.install_graph(error),error.c_str());
        CHECK(!provider.ensure_part_surface(asset_hash,inputs,error) && error=="injected stream material failure",
              "streamed material publication failure is reported on a geometry cache hit");
    }
    std::printf("STREAM_PART_SURFACE outside_graph=checked cold_warm=published failure=reported\n");
    write(root/"objects/Assembly.js",R"(
class Assembly extends Part {
 static requires(){return [{module:'Stone',params:{}},{module:'Plain',params:{}}];}static noImpostor=true;
 build(){this.fill(8);this.beginShape(0);
  this.vertex(-4,0,-4);this.vertex(4,0,4);this.vertex(4,0,-4);this.endShape();
  this.placeChild('Stone',{}, {instanced:true,inlineBelowPx:0});
  this.translate(1,0,0);this.placeChild('Stone',{}, {instanced:true,inlineBelowPx:0});
  this.translate(1,0,0);this.placeChild('Plain',{}, {instanced:true,inlineBelowPx:64});
 }
})");
    write(root/"objects/Plain.js",R"(
class Plain extends Part {static noImpostor=true;build(){this.fill(8);this.beginShape(0);
 this.vertex(-.04,0,0);this.vertex(.04,0,0);this.vertex(0,.04,0);this.endShape();}}
)");
    write(root/"worlds/AssemblyWorld.js","class AssemblyWorld extends World {static roots=[{module:'Assembly',params:{}}];}");
    auto assembly_cfg=viewer::LocalProviderConfig::for_project(root.string(),"AssemblyWorld","");
    assembly_cfg.publish_part_surface=[&](auto value,std::string&){direct_active=value;return true;};
    {
        viewer::LocalProvider provider(assembly_cfg);
        const bool connected=provider.connect(world,error);
        CHECK(connected,error.c_str());
        CHECK(connected && world.instances.size()==3,"one authored root plus two permanent material instances");
        if(connected && !world.instances.empty()) {
            const auto hash=world.instances[0].part_hash;
            CHECK(provider.ensure_part_flattened(hash),"material-bearing child assembly flattens successfully");
            BLASManager blas;TLASManager tlas(16);
            std::vector<part_asset::FlatCluster> clusters;std::vector<part_asset::FlatInstanceRef> refs;
            const auto path=assembly_cfg.cache_root+"/"+part_asset::cache_path_flat(hash);
            CHECK(part_asset::load_flat_v3(path,hash,blas,tlas,clusters,refs),"flattened assembly reloads");
            CHECK(refs.size()==3,"permanent and distance-limited children retain separate reference contracts");
            std::vector<float> permanent_positions;unsigned temporary=0;
            for(const auto& ref:refs) {
                if(ref.inline_cutover>0){++temporary;continue;}
                CHECK(direct_active && ref.child_resolved_hash==direct_active->part_hash,
                      "all LODs retain the same shared material owner");
                permanent_positions.push_back(ref.transform[3]);
            }
            CHECK(temporary==1 && permanent_positions==std::vector<float>({0,1}),
                  "other children's coarse cutover cannot inline either textured rock");
        }
    }
    std::printf("DIRECT_PART_ASSEMBLY permanent_refs=2 shared_source=1\n");
}
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);try{run();}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}return check_summary();}
