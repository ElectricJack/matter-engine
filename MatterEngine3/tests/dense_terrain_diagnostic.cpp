// Bounded production-path probe. Run from repository root with an EMPTY output
// directory. It uses the authored mountain field and sector, without scatter.
#include "script_host.h"
#include "terrain_field.h"
#include "render/part_store.h"
#include "bake_trace.h"
#include "script/world_definition_loader.h"
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>

static std::string read(const char* p) { std::ifstream f(p);std::ostringstream s;s<<f.rdbuf();return s.str(); }
using Clock=std::chrono::steady_clock;
static double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
static void spans(const bake_trace::Span& s,int depth=0) {
    if(s.name)std::printf("span depth=%d name=%s ms=%.3f\n",depth,s.name,s.end_ms-s.begin_ms);
    for(const auto& c:s.children)spans(c,depth+1);
}
int main(int argc,char** argv) {
    if(argc!=2){std::fprintf(stderr,"usage: dense_terrain_diagnostic EMPTY_OUTPUT_DIR\n");return 2;}
    const std::string dir=argv[1];
    if(std::filesystem::exists(dir)){std::fprintf(stderr,"output must not exist\n");return 2;}
    std::filesystem::create_directories(dir);
    _putenv_s("MATTER_GEOMETRY_TERRAIN","1");
    _putenv_s("MATTER_GEOMETRY_PAGES","1");
    _putenv_s("MATTER_GEOMETRY_PAGES_PROFILE","1");
    _putenv_s("MATTER_GEOMETRY_ROOT_MB","256");
    script_host::ScriptHost host;
    host.set_shared_lib_roots({"projects/world_demo/shared-lib","MatterEngine3/shared-lib"});
    matter::WorldLoadDesc desc;desc.world_path="projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js";
    desc.objects_dir="projects/world_demo/objects";desc.project_shared_lib_dir="projects/world_demo/shared-lib";
    desc.engine_shared_lib_dir="MatterEngine3/shared-lib";
    matter::WorldDefinition definition;matter::WorldLoadError load_error;
    if(!matter::load_world_definition(desc,definition,load_error)){std::fprintf(stderr,"definition: %s\n",load_error.message.c_str());return 1;}
    const auto world=host.eval_world(read("projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js"),"{}");
    if(!world.ok){std::fprintf(stderr,"world: %s\n",world.message.c_str());return 1;}
    terrain_field::FieldProgram program;std::string error;
    if(!terrain_field::FieldProgram::parse(world.field_program,program,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
    terrain_field::FieldRuntime field(std::move(program));
    const auto source=read("projects/world_demo/scenes/streaming/StreamMountain/objects/WorldSector.js");
    // Same 64m cube under the inspection camera; keep source resolution 0.25m.
    const int ty=int(std::floor(field.height_at(425,1456)/64.f));
    const std::string params="{\"tx\":6,\"ty\":"+std::to_string(ty)+",\"tz\":22,\"terrainLod\":5,\"volumetric\":1,\"sectorSize\":64,\"worldSeed\":20260722,\"biomes\":\"{\\\"__terrainOnly\\\":true}\"}";
    script_host::BakeOptions options;options.parts_dir=dir;options.retain_geometry=true;
    options.world.field=&field;options.world.sector_size=64;options.world.y_min=-96;options.world.y_max=704;
    bake_trace::Collector trace;bake_trace::set_current(&trace);
    auto t=Clock::now();auto baked=host.bake_source(source,params,options);
    std::printf("diagnostic bake_ms=%.3f ty=%d\n",ms(t),ty);
    bake_trace::set_current(nullptr);spans(trace.snapshot());
    if(!baked.error.ok||!baked.geometry){std::fprintf(stderr,"bake: %s\n",baked.error.message.c_str());return 1;}
    for(int pass=0;pass<2;++pass) {
        // New store makes the warm pass reopen disk/index, not reuse RAM roots.
        viewer::PartStore store(dir);
        store.set_geometry_pages_enabled(true);
        viewer::WarpAnchor warp;warp.valid=true;warp.x=384;warp.z=1408;warp.sector_size=64;warp.base_sector_size=64;
        t=Clock::now();auto staged=store.stage_from_bake(baked.resolved_hash,*baked.geometry,0,true,warp,128);
        std::printf("diagnostic pass=%s stage_ms=%.3f prep_ms=%.3f ladder_ms=%.3f tail_ms=%.3f warp_ms=%.3f ok=%d pages=%d\n",
            pass?"warm":"cold",ms(t),staged.prep_ms,staged.ladder_ms,staged.tail_ms,staged.warp_ms,staged.ok,int(bool(staged.lp.geometry_pages)));
        if(!staged.ok||!staged.lp.geometry_pages)return 1;
    }
    return 0;
}
