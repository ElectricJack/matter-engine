#include "check.h"
#include "finite_surface_faces.h"
#include "terrain_field.h"
#include "render/part_store.h"
#include "render/vt_surface_tape.h"
#include "blas_manager.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;
namespace {
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string recipe(const std::string& change = "", const std::string& body = "return d;") {
    return "class Finite extends Part { static params={size:.1,tone:.3};"
        "constructor(){super();throw Error('constructor ran');}"
        "build(){throw Error('build ran');} static finiteSurface(p){const d={"
        "version:1,material:8,pixelM:.002,boundsMinM:[-.1,-.1,-.1],boundsMaxM:[.1,.1,.1],"
        "solid:{version:1,voxelM:.005,ops:[{shape:'box',halfExtentsM:[p.size,.1,.1]}]},"
        "appearance:s=>({baseColor:[s.x.mul(.1).add(p.tone),.1,.05],roughness:.8,"
        "height:0,heightRange:[0,0]}),base:s=>({baseColor:[.1,.1,.1],roughness:.9,"
        "height:0,heightRange:[0,0]})};" + change + body + "}}";
}
void run() {
    const auto repo = fs::current_path();
    const auto temp = fs::temp_directory_path() / ("matter-finite-source-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    struct Cleanup {
        fs::path original, temporary;
        ~Cleanup() { fs::current_path(original); std::error_code e; fs::remove_all(temporary, e); }
    } cleanup{repo, temp};
    fs::current_path(temp);
    script_host::ScriptHost host;
    {
        const auto source=[](const std::string& edit="") {
            return "class Direct extends Part {static params={tone:.3};"
                "constructor(){super();throw Error('constructor ran');} build(){throw Error('build ran');}"
                "static surface(p){const d={version:1,material:8,recipe:s=>({"
                "baseColor:[s.x.mul(.01).add(p.tone),.1,.05],roughness:.8,"
                "height:s.noise3(71,3,1).mul(.001).sub(.002),heightRange:[-.003,-.001]})};"+edit+"return d;}}";
        };
        script_host::EvaluatedDirectSurface direct;script_host::EvaluatedFiniteSurface finite;
        script_host::BakeError e;
        // Avoid any separate test-side tape builder: exercise native JS metadata.
        CHECK(host.evaluate_part_surface(source(),"{}",direct,finite,e),e.message.c_str());
        CHECK(direct.present && !finite.present && !host.last_build_ran(),"direct metadata does not construct/build");
        CHECK(direct.part_hash==host.resolve_hash(source(),"{}"),"direct material and geometry share content identity");
        const auto first=direct;
        CHECK(host.evaluate_part_surface(source(),"{\"tone\":0.6}",direct,finite,e),e.message.c_str());
        CHECK(direct.part_hash!=first.part_hash && direct.program_hash!=first.program_hash,"material edits change both identities");
        const auto valid=direct;
        for(const auto& edit:{"d.version=2;","d.extra=1;","d.material=8.5;","d.material=256;","d.recipe=null;",
            "d.recipe=s=>({baseColor:[s.worldX,0,0],roughness:1,height:0,heightRange:[0,0]});",
            "d.recipe=s=>({baseColor:[NaN,0,0],roughness:1,height:0,heightRange:[0,0]});",
            "d.recipe=s=>({baseColor:[.1,0,0],roughness:1,height:s.normalY.mul(.001),heightRange:[-.001,.001],heightContext:'receiver'});",
            "d.recipe=s=>{s.weight(9,1);return {baseColor:[.1,0,0],roughness:1,height:0,heightRange:[0,0]};};"}) {
            CHECK(!host.evaluate_part_surface(source(edit),"{}",direct,finite,e),edit);
            CHECK(direct.part_hash==valid.part_hash && direct.program==valid.program,"rejected recipe preserves prior output");
        }
        auto dual=source();dual.insert(dual.find("static params"),"static finiteSurface(){} ");
        CHECK(!host.evaluate_part_surface(dual,"{}",direct,finite,e),"ambiguous declarations rejected");
        auto child=source();child.insert(child.find("static params"),"static requires=['Child']; ");
        CHECK(!host.evaluate_part_surface(child,"{}",direct,finite,e),"shared material requires standalone geometry");
        script_host::SolidSourceEvaluationOptions options;options.time_budget_ms=5;
        CHECK(!host.evaluate_part_surface(source("while(true){}"),"{}",direct,finite,e,options),"direct recipe has deadline");
        options={};options.control.generation_is_current=[](uint64_t){return false;};
        CHECK(!host.evaluate_part_surface(source(),"{}",direct,finite,e,options),"direct recipe rejects stale generation");
        CHECK(host.evaluate_part_surface(recipe(),"{}",direct,finite,e) && finite.present && !direct.present,
              "generic discovery preserves finite-source path");
        CHECK(host.evaluate_part_surface("class Legacy extends Part {}","{}",direct,finite,e) && !direct.present && !finite.present,
              "legacy absence clears both complete outputs");
    }
    unsigned gpu_calls = 0;
    host.set_solid_source_baker([&](const auto&, auto&, auto&, auto&, const auto&) {
        ++gpu_calls; return false;
    });
    script_host::EvaluatedFiniteSurface result;
    script_host::BakeError error;
    CHECK(host.evaluate_finite_surface(recipe(), "{}", result, error), error.message.c_str());
    CHECK(result.present && !host.last_build_ran(), "static recipe never constructs/builds part");
    CHECK(result.geometry.resolved_hash == host.resolve_hash(recipe(), "{}"),
          "receiver and declaration use the same resolved hash");
    const auto old = result;
    CHECK(host.evaluate_finite_surface(recipe(), "{\"tone\":0.6}", result, error), error.message.c_str());
    CHECK(result.appearance_hash != old.appearance_hash && result.base_hash == old.base_hash &&
          result.geometry.recipe_digest == old.geometry.recipe_digest &&
          result.geometry.resolved_hash != old.geometry.resolved_hash,
          "appearance edits invalidate material/part identity while preserving source geometry");
    CHECK(host.evaluate_finite_surface(recipe(), "{\"size\":0.09}", result, error), error.message.c_str());
    CHECK(result.geometry.recipe_digest != old.geometry.recipe_digest,
          "physical geometry edit changes reusable geometry identity");
    const auto valid = result;
    for (const auto& change : {
        "d.version=2;", "d.unknown=1;", "d.pixelM=0;", "d.pixelM=0.00001;",
        "d.boundsMinM=[0,0,0];d.boundsMaxM=[0,0,0];", "d.boundsMaxM=[NaN,1,1];",
        "d.boundsMaxM=[100,100,100];", "d.material=8.5;", "d.material=65536;",
        "d.solid.unknown=1;", "d.solid.ops[0].halfExtentsM=[-1,1,1];",
        "d.appearance=s=>({baseColor:[s.worldX,0,0],roughness:.5,height:0,heightRange:[0,0]});",
        "d.base=s=>({baseColor:[s.worldX,0,0],roughness:.5,height:0,heightRange:[0,0]});",
        "d.appearance=s=>({baseColor:[NaN,0,0],roughness:.5,height:0,heightRange:[0,0]});",
        "d.appearance=()=>{throw Error('appearance failure');};",
        "d.appearance=s=>{s.weight(9,1);return d.base(s);};",
        "d.base=null;"}) {
        CHECK(!host.evaluate_finite_surface(recipe(change), "{}", result, error), change);
        CHECK(result.geometry.resolved_hash == valid.geometry.resolved_hash &&
              result.appearance_program == valid.appearance_program,
              "failure preserves the prior complete owned recipe");
    }
    CHECK(host.evaluate_finite_surface("class Legacy extends Part {build(){throw Error('ran');}}",
                                      "{}", result, error) && !result.present,
          "missing declaration is a valid opt-out without build");
    CHECK(!host.evaluate_finite_surface("class Invalid extends Part {static finiteSurface=1;}",
                                       "{}", result, error), "malformed declaration is not an opt-out");
    auto dependency = recipe(); dependency.insert(dependency.find("static params"), "static requires=['Child'];");
    CHECK(!host.evaluate_finite_surface(dependency, "{}", result, error), "child source rejected");
    script_host::SolidSourceEvaluationOptions options;
    options.time_budget_ms = 5;
    CHECK(!host.evaluate_finite_surface(recipe("while(true){}"), "{}", result, error, options),
          "recipe execution obeys deadline");
    CHECK(!host.evaluate_finite_surface("while(true){};"+recipe(), "{}", result, error, options),
          "top-level source also obeys deadline");
    options = {}; unsigned polls = 0;
    options.control.cancelled = [&] { return ++polls > 4; };
    CHECK(!host.evaluate_finite_surface(recipe("while(true){}"), "{}", result, error, options) &&
          error.code == "solid-source-cancelled", "active recipe cancellation");
    options = {}; options.control.generation_is_current = [](uint64_t) { return false; };
    CHECK(!host.evaluate_finite_surface(recipe(), "{}", result, error, options), "stale recipe rejected");
    CHECK(gpu_calls == 0 && fs::is_empty(temp), "all evaluations are artifact-free and do not mesh");

    host.set_shared_lib_roots({(repo/"projects/world_demo/shared-lib").string(),
                               (repo/"MatterEngine3/shared-lib").string()});
    const auto clay = read(repo/"projects/world_demo/objects/texturing/bricks/ClayBrickSurface.js");
    CHECK(!clay.empty(), "actual JS receiver source available; run from repository root");
    std::set<uint64_t> fields, materials;
    for (unsigned seed = 0; seed < 8 && !clay.empty(); ++seed) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = host.evaluate_finite_surface(clay, "{\"seed\":"+std::to_string(seed)+"}", result, error);
        CHECK(ok, error.message.c_str()); if (!ok) continue;
        fields.insert(result.geometry.recipe_digest); materials.insert(result.appearance_hash);
        std::array<script_host::FiniteSurfaceFace, 6> faces;
        gpu_meshing::Error face_error;
        CHECK(script_host::plan_finite_surface_faces(result, faces, face_error), face_error.message.c_str());
        for (const auto& f : faces) {
            CHECK(f.geometry.source.ops == result.geometry.source.ops.data(), "face jobs borrow owned op tape");
            const auto& n = f.geometry.frame.n;
            const float normal[] = {n.x,n.y,n.z};
            const auto& o = f.geometry.frame.origin_m;
            const float center[] = {o.x,o.y,o.z};
            for (unsigned axis=0; axis<3; ++axis) if (normal[axis] != 0)
                CHECK(std::abs(center[axis]+normal[axis]*f.datum_m -
                      (normal[axis]>0 ? result.bounds_max_m[axis] : result.bounds_min_m[axis])) < 1e-7f,
                      "all receiver datum planes match authored bounds");
        }
        const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::printf("FINITE_RECIPE seed=%u ops=%zu faces=6 eval_ms=%.4f gpu_calls=0 artifact_writes=0\n",
                    seed, result.geometry.source.ops.size(), elapsed);
    }
    CHECK(fields.size()==8 && materials.size()==8, "all eight real bricks own distinct geometry/material recipes");
    CHECK(host.evaluate_finite_surface(clay, "{}", result, error), error.message.c_str());
    const auto original_clay = result;
    CHECK(host.evaluate_finite_surface(clay, "{\"red\":0.35}", result, error), error.message.c_str());
    CHECK(result.geometry.recipe_digest == original_clay.geometry.recipe_digest &&
          result.appearance_hash != original_clay.appearance_hash, "real clay color edit reuses source geometry");
    const auto wall=read(repo/"projects/world_demo/objects/texturing/bricks/ClayBrickWallSurface.js");
    script_host::EvaluatedFiniteSurface small_wall, large_wall;
    CHECK(host.evaluate_finite_surface(wall,"{\"columns\":2,\"courses\":3}",small_wall,error),error.message.c_str());
    CHECK(host.evaluate_finite_surface(wall,"{\"columns\":4,\"courses\":6}",large_wall,error),error.message.c_str());
    CHECK(small_wall.version==2 && small_wall.sources.size()==8 && small_wall.placements.size()==12,
          "whole wall declares reusable sources and placements without child geometry");
    CHECK(large_wall.sources.size()==small_wall.sources.size(),"resize preserves source bank size");
    for(size_t i=0;i<small_wall.sources.size() && i<large_wall.sources.size();++i)
        CHECK(small_wall.sources[i].geometry.recipe_digest==large_wall.sources[i].geometry.recipe_digest &&
              small_wall.sources[i].appearance_hash==large_wall.sources[i].appearance_hash,
              "resizing wall does not change reusable geometry or material identity");
    const auto composite_body=[](const std::string& edit) {
        return "const c={version:2,material:8,pixelM:.002,boundsMinM:[-.1,-.1,-.1],boundsMaxM:[.1,.1,.1],"
            "base:d.base,sources:[d],placements:[{id:'a',source:0,matrix:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]};"+edit+"return c;";
    };
    CHECK(host.evaluate_finite_surface(recipe("",composite_body("")),"{}",result,error),error.message.c_str());
    for(const auto& edit:{"c.placements[0].matrix[0]=2;","c.placements[0].matrix[0]=-1;",
        "c.placements[0].matrix[3]=1;","c.placements[0].source=4;","c.placements.push(c.placements[0]);",
        "c.placements.push({...c.placements[0],id:'overlap'});",
        "c.placements[0].matrix=[.70710678,0,.70710678,0,0,1,0,0,-.70710678,0,.70710678,0,0,0,0,1];",
        "c.sources=[c];","c.placements=[];","c.sources=[];","c.pixelM=.00001;"})
        CHECK(!host.evaluate_finite_surface(recipe("",composite_body(edit)),"{}",result,error),edit);
    const std::string periodic_body=
        "const frame={originM:[-.1,-.1,.1],u:[1,0,0],v:[0,1,0],n:[0,0,1]};"
        "c.periodic={modules:[{...frame,periodM:[.2,.2],texelsPerM:64,base:d.base,"
        "placements:c.placements}],mappings:[{...frame,module:0,phase:[-.5,.25],uRangeM:[.01,.19]}]};";
    CHECK(host.evaluate_finite_surface(recipe("",composite_body(periodic_body)),"{}",result,error),error.message.c_str());
    CHECK(result.modules.size()==1 && result.modules[0].placements.size()==1 &&
          result.material_mappings.size()==1 && result.material_mappings[0].phase[0]==-.5f &&
          result.material_mappings[0].datum_m==0,"generic modules retain owned placements and bounded phased mappings");
    const auto periodic_valid=result;
    for(const auto& edit:{"c.periodic.modules=[];","c.periodic.mappings=[];",
        "c.periodic.modules[0].u=[2,0,0];","c.periodic.modules[0].n=[0,0,-1];",
        "c.periodic.modules[0].periodM=[0,.2];","c.periodic.modules[0].texelsPerM=Infinity;",
        "c.periodic.modules[0].periodM=[1000,.2];","c.periodic.modules[0].unknown=1;",
        "c.periodic.modules[0].placements[0].source=99;",
        "c.periodic.modules[0].placements[0].matrix[0]=2;",
        "c.periodic.modules[0].base=s=>({baseColor:[s.worldX,0,0],roughness:1,height:0,heightRange:[0,0]});",
        "c.periodic.mappings[0].module=2;","c.periodic.mappings[0].phase=[NaN,0];",
        "c.periodic.mappings[0].datumM=Infinity;","c.periodic.mappings[0].uRangeM=[.2,.1];",
        "c.periodic.mappings[0].v=[1,0,0];","c.periodic.mappings[0].unknown=1;"}) {
        CHECK(!host.evaluate_finite_surface(recipe("",composite_body(periodic_body+edit)),"{}",result,error),edit);
        CHECK(result.geometry.resolved_hash==periodic_valid.geometry.resolved_hash && result.modules.size()==1 &&
              result.modules[0].base_program==periodic_valid.modules[0].base_program &&
              result.material_mappings[0].phase==periodic_valid.material_mappings[0].phase,
              "invalid module or mapping preserves the previous complete recipe");
    }
    for(const auto& params:{"{\"moduleColumns\":8,\"moduleCourses\":4,\"columns\":8,\"courses\":4}",
        "{\"moduleColumns\":8,\"moduleCourses\":4,\"columns\":16,\"courses\":8,\"phaseColumns\":-3,\"phaseCourses\":-2}"}) {
        CHECK(host.evaluate_finite_surface(wall,params,result,error),error.message.c_str());
        CHECK(result.modules.size()==2 && result.material_mappings.size()==2 &&
              result.modules[0].placements.size()==32 && result.modules[1].placements.size()==32,
              "actual brick walls compile two fixed modules independent of receiver extent and phase");
    }
    const auto path_wall=read(repo/"projects/world_demo/objects/texturing/bricks/ClayBrickWallPath.js");
    for(const auto& params:{"{\"kind\":\"corner\",\"columns\":32,\"returnColumns\":32,\"courses\":24}",
        "{\"kind\":\"u\",\"columns\":40,\"returnColumns\":16,\"courses\":16}",
        "{\"kind\":\"curve\",\"columns\":20,\"courses\":16}",
        "{\"kind\":\"curve\",\"columns\":14,\"centerJointM\":0.02,\"courses\":24}"}) {
        const bool ok=host.evaluate_finite_surface(path_wall,params,result,error);
        CHECK(ok,error.message.c_str());
        if(ok) CHECK(result.version==3 && result.receivers.size()>=6 && result.sources.size()==8,
            "maze paths declare explicit metric receiver planes and one reusable bank");
        std::printf("PATH_RECIPE placements=%zu receivers=%zu ok=%d\n",result.placements.size(),result.receivers.size(),ok);
    }
    const std::string weathered_maze=
        "import {clayBrickMazeWalls} from 'shared-lib/clay_brick_maze_layout';"
        "import {wallWeatherSeed} from 'shared-lib/wall_weathering';"
        "import {clayBrickWallPathSurface} from 'shared-lib/clay_brick_wall_path';"
        "import {clayBrickWallSurface} from 'shared-lib/clay_brick_wall_surface';"
        "class Weathered extends Part {static finiteSurface(p){const w=clayBrickMazeWalls()[p.index];"
        "const q={...w.params,weathering:1,weatherSeed:wallWeatherSeed(w.id),graffiti:1};"
        "return q.kind?clayBrickWallPathSurface(q):clayBrickWallSurface(q);}}";
    std::set<uint64_t> weathered_recipes;
    for(unsigned index=0;index<20;++index) {
        const bool ok=host.evaluate_finite_surface(weathered_maze,"{\"index\":"+std::to_string(index)+"}",result,error);
        CHECK(ok,error.message.c_str());if(!ok) continue;
        terrain_field::SurfaceProgram program;std::string why;vt::VtSurfaceTapePack packed;
        CHECK(terrain_field::SurfaceProgram::parse(result.base_program,program,why),why.c_str());
        CHECK(program.has_coat() && vt::vt_pack_surface_tape(program,false,packed),
              "actual maze weathering including graffiti fits bounded GPU tape/register budgets");
        weathered_recipes.insert(program.hash());
        std::printf("WEATHER_RECIPE index=%u ops=%zu\n",index,program.ops.size());
    }
    CHECK(weathered_recipes.size()==20,"maze owners compile distinct stable weathering");
    const std::string explicit_receiver="c.version=3;c.receivers=[{originM:[0,.1,0],u:[1,0,0],v:[0,0,-1],n:[0,1,0],domainM:[-.2,-.2,.4,.4]}];";
    for(const auto& change:{"c.receivers=[];","c.receivers[0].u=[2,0,0];","c.receivers[0].domainM[2]=-1;",
        "c.placements.push({...c.placements[0],id:'overlap'});"})
        CHECK(!host.evaluate_finite_surface(recipe("",composite_body(explicit_receiver+change)),"{}",result,error),change);
    // The shared recorder must compile identical appearance semantics in both
    // entry points, with independent register numbering for the base recipe.
    const auto world = host.eval_world(
        "import {clayBrickMaterial} from 'shared-lib/clay_brick_material';"
        "class W extends World {field(){const z=noise2(1,.01,1).mul(0);return {"
        "density:heightToDensity(z),moisture:z,relief:z,seaLevel:-10};}"
        "surfaces(s){s.source(8,clayBrickMaterial(s,0,[.28,.090,.041]));}}", "{}");
    CHECK(world.ok, world.message.c_str());
    terrain_field::SurfaceProgram parsed; std::string parse_error;
    CHECK(terrain_field::SurfaceProgram::parse(world.surface_program, parsed, parse_error), parse_error.c_str());
    CHECK(parsed.hash() == original_clay.appearance_hash, "Part/World share identical surface tape semantics");
    CHECK(gpu_calls==0 && fs::is_empty(temp), "real imported recipes stay artifact-free");
    script_host::BakeOptions bake_options;
    bake_options.output_mode = script_host::BakeOptions::OutputMode::RuntimeLeafMemory;
    bake_options.parts_dir = (temp / "must-not-exist").string();
    const auto baked = host.bake_source(clay, "{}", bake_options);
    CHECK(baked.error.ok && baked.geometry && baked.geometry->blas, baked.error.message.c_str());
    size_t triangles = 0;
    if (baked.geometry && baked.geometry->blas)
        for (const auto& entry : baked.geometry->blas->get_entries())
            if (entry) triangles += entry->triangles.size();
    CHECK(triangles == 12 && baked.resolved_hash == original_clay.geometry.resolved_hash,
          "actual native receiver build emits 12 triangles under the same recipe identity");
    CHECK(gpu_calls==0 && fs::is_empty(temp), "receiver needs neither detailed source meshing nor artifacts");
    for(const auto& params:{"{\"bond\":\"stack\",\"columns\":2,\"courses\":3}",
        "{\"columns\":4,\"courses\":5}","{\"widthM\":0.5,\"heightM\":0.6}"}) {
        const auto built=host.bake_source(wall,params,bake_options);
        CHECK(built.error.ok && built.geometry && built.geometry->blas,built.error.message.c_str());
        size_t count=0;
        if(built.geometry && built.geometry->blas) for(const auto& entry:built.geometry->blas->get_entries())
            if(entry) count+=entry->triangles.size();
        CHECK(count==12,"every complete wall is one 12-triangle receiver");
        std::printf("WALL_RECEIVER triangles=%zu source_mesh_calls=%u\n",count,gpu_calls);
    }
    for(const auto& test:{std::make_pair("corner",20u),std::make_pair("u",28u),std::make_pair("curve",164u)}) {
        const auto params=std::string("{\"kind\":\"")+test.first+"\",\"columns\":20,\"courses\":16}";
        const auto built=host.bake_source(path_wall,params,bake_options);
        CHECK(built.error.ok && built.geometry && built.geometry->blas,built.error.message.c_str());
        size_t count=0;
        if(built.geometry && built.geometry->blas) for(const auto& entry:built.geometry->blas->get_entries())
            if(entry) count+=entry->triangles.size();
        CHECK(count==test.second,"maze path shell triangle count is independent of brick courses");
        std::printf("PATH_RECEIVER kind=%s triangles=%zu\n",test.first,count);
    }
    std::printf("FINITE_RECEIVER triangles=%zu source_mesh_calls=%u artifact_writes=0\n", triangles, gpu_calls);
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { run(); } catch (const std::exception& e) { std::printf("FAIL: %s\n",e.what()); return 1; }
    return check_summary();
}
