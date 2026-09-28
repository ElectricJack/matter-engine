#include "part_surface.h"
#include "finite_surface_faces.h"
#include "render/vt_periodic_material.h"
#include <cstdio>
#include <filesystem>
namespace part_surface {
bool prepare_direct(const script_host::EvaluatedDirectSurface& recipe,
    std::shared_ptr<const Prepared>& out,gpu_meshing::Error& error,
    const gpu_meshing::BuildControl& control) {
    using namespace gpu_meshing;
    if (control.cancelled && control.cancelled()) {
        error={ErrorCode::Cancelled,"part surface cancelled"};return false;
    }
    if (control.generation_is_current && !control.generation_is_current(recipe.generation)) {
        error={ErrorCode::StaleGeneration,"part surface generation changed"};return false;
    }
    if (!recipe.present || !recipe.part_hash || recipe.program.empty()) {
        error={ErrorCode::InvalidInput,"direct part surface is incomplete"};return false;
    }
    auto result=std::make_shared<Prepared>();result->kind=Prepared::Kind::Direct;
    result->part_hash=recipe.part_hash;result->base_hash=recipe.program_hash;
    result->base_program=recipe.program;result->material=recipe.material;
    out=std::move(result);error={};return true;
}
bool prepare(const script_host::EvaluatedFiniteSurface& recipe, const std::string& root,
    const gpu_meshing::SolidFaceProjector& project, const MaterialBaker& shade,
    std::shared_ptr<const Prepared>& out, Stats& stats, gpu_meshing::Error& error,
    const gpu_meshing::BuildControl& control, SourceCache* source_cache) {
    using namespace gpu_meshing;
    stats = {}; error = {};
    const auto current = [&] {
        if (control.cancelled && control.cancelled()) {
            error={ErrorCode::Cancelled,"part surface cancelled"}; return false;
        }
        if (control.generation_is_current && !control.generation_is_current(recipe.geometry.generation)) {
            error={ErrorCode::StaleGeneration,"part surface generation changed"}; return false;
        }
        return true;
    };
    if (!current()) return false;
    if (!shade) { error={ErrorCode::Unavailable,"part surface requires material bake service"};return false; }
    std::array<script_host::FiniteSurfaceFace,6> faces;
    if (!script_host::plan_finite_surface_faces(recipe,faces,error)) return false;
    if (root.empty()) {error={ErrorCode::InvalidInput,"finite source cache root is empty"};return false;}
    if (recipe.version>=2) {
        // Admit the full reusable bank before allocating projected/material
        // images. Conservative mip storage bound (less than 2x base pixels).
        uint64_t source_bytes=0;
        for(const auto& source:recipe.sources) {
            std::array<script_host::FiniteSurfaceFace,6> source_faces;
            if(!script_host::plan_finite_surface_faces(source,source_faces,error)) return false;
            for(const auto& face:source_faces) {
                FaceLayout layout;std::vector<FaceRegion> regions;
                if(!plan_face_regions(face.geometry,{},layout,regions,error)) return false;
                source_bytes+=uint64_t(layout.width)*layout.height*2*sizeof(surface_stamp::Channels);
            }
            if(source_bytes>128u*1024u*1024u) {
                error={ErrorCode::InvalidInput,"composite source bank exceeds 128 MiB preparation budget"};return false;
            }
        }
        std::vector<std::shared_ptr<const Prepared>> bank;
        for (const auto& source:recipe.sources) {
            std::shared_ptr<const Prepared> prepared; Stats child;
            if (!prepare(source,root,project,shade,prepared,child,error,control,source_cache)) return false;
            stats.geometry_hits+=child.geometry_hits;stats.material_hits+=child.material_hits;stats.faces+=child.faces;
            bank.push_back(std::move(prepared));
        }
        auto result=std::make_shared<Prepared>();
        result->part_hash=recipe.geometry.resolved_hash;
        result->base_hash=recipe.base_hash;result->base_program=recipe.base_program;
        result->material=recipe.geometry.source.material;
        std::vector<vt::VtFiniteReceiver> receivers;
        if(recipe.version==3) {
            const auto vec=[](const auto& a){return matter::Float3{a[0],a[1],a[2]};};
            for(const auto& r:recipe.receivers) receivers.push_back({
                {vec(r.origin_m),vec(r.u),vec(r.v),vec(r.n)},0,r.domain_m,{}});
        } else for(const auto& f:faces) receivers.push_back({f.geometry.frame,f.datum_m,
            {f.geometry.u_min_m,f.geometry.v_min_m,f.geometry.u_max_m-f.geometry.u_min_m,
             f.geometry.v_max_m-f.geometry.v_min_m},{}});
        std::vector<std::shared_ptr<const surface_stamp::Stamp>> payload_bank;
        for(const auto& source:bank) payload_bank.insert(payload_bank.end(),
            source->sources->payloads.begin(),source->sources->payloads.end());
        std::vector<vt::VtFiniteSourceBinding> bindings;
        auto dot=[](matter::Float3 a,matter::Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
        for(const auto& p:recipe.placements) {
            if(!current()) return false;
            const auto& m=p.matrix;
            const auto transform=[&](const float* x,bool point) {
                matter::Float3 v{m[0]*x[0]+m[1]*x[1]+m[2]*x[2],
                    m[4]*x[0]+m[5]*x[1]+m[6]*x[2],m[8]*x[0]+m[9]*x[1]+m[10]*x[2]};
                if(point){v.x+=m[3];v.y+=m[7];v.z+=m[11];}return v;
            };
            const auto& catalog=*bank[p.source]->sources;
            for(size_t b=0;b<catalog.bindings.size();++b) {
                const auto& original=catalog.bindings[b];
                FaceFrame frame{transform(original.origin_datum,true),transform(original.u,false),
                    transform(original.v,false),transform(original.n,false)};
                for(auto& r:receivers) {
                    const matter::Float3 delta{frame.origin_m.x-r.frame.origin_m.x,
                        frame.origin_m.y-r.frame.origin_m.y,frame.origin_m.z-r.frame.origin_m.z};
                    if(dot(frame.n,r.frame.n)<.9999f ||
                        std::abs(dot(delta,r.frame.n)+original.origin_datum[3]-r.datum_m)>.00005f) continue;
                    // Find the payload by its first packed mip offset.
                    std::shared_ptr<const surface_stamp::Stamp> stamp;
                    size_t first=0;
                    for(const auto& s:catalog.payloads) {
                        if(first==original.levels[0]) {stamp=s;break;} first+=s->levels.size();
                    }
                    if(!stamp){error={ErrorCode::InvalidInput,"composite source payload missing"};return false;}
                    r.sources.push_back(uint32_t(bindings.size()));
                    bindings.push_back({std::move(stamp),frame,original.origin_datum[3]});
                }
            }
        }
        std::string message;
        if(!vt::vt_make_finite_sources(bindings,result->sources,message,receivers,payload_bank)) {
            error={ErrorCode::InvalidInput,message};return false;
        }
        // Periodic material inputs reuse the already prepared source bank.
        // Their identity contains canonical placements/materials, never the
        // finite receiver's dimensions, chart packing or instance transform.
        const auto vec=[](const auto& a){return matter::Float3{a[0],a[1],a[2]};};
        const auto frame_of=[&](const auto& f){return FaceFrame{vec(f.origin_m),vec(f.u),vec(f.v),vec(f.n)};};
        for(const auto& module:recipe.modules) {
            if(!current())return false;
            const FaceFrame module_frame=frame_of(module.frame);
            vt::VtPeriodicDomain domain;
            if(!vt::vt_make_periodic_domain(module_frame,module.period_m,module.texels_per_m,domain,message)) {
                error={ErrorCode::InvalidInput,message};return false;
            }
            std::vector<vt::VtFiniteSourceBinding> placed;
            for(const auto& p:module.placements) {
                const auto& m=p.matrix;
                const auto transform=[&](const float* x,bool point) {
                    return matter::Float3{m[0]*x[0]+m[1]*x[1]+m[2]*x[2]+(point?m[3]:0),
                        m[4]*x[0]+m[5]*x[1]+m[6]*x[2]+(point?m[7]:0),
                        m[8]*x[0]+m[9]*x[1]+m[10]*x[2]+(point?m[11]:0)};
                };
                const auto& catalog=*bank[p.source]->sources;
                const auto before=placed.size();
                for(const auto& original:catalog.bindings) {
                    FaceFrame frame{transform(original.origin_datum,true),transform(original.u,false),
                        transform(original.v,false),transform(original.n,false)};
                    const matter::Float3 delta{frame.origin_m.x-module_frame.origin_m.x,
                        frame.origin_m.y-module_frame.origin_m.y,frame.origin_m.z-module_frame.origin_m.z};
                    if(dot(frame.n,module_frame.n)<.9999f ||
                       std::abs(dot(delta,module_frame.n)+original.origin_datum[3])>.00005f)continue;
                    size_t first=0;std::shared_ptr<const surface_stamp::Stamp> payload;
                    for(const auto& stamp:catalog.payloads) {
                        if(first==original.levels[0]){payload=stamp;break;}first+=stamp->levels.size();
                    }
                    if(!payload){error={ErrorCode::InvalidInput,"periodic source payload missing"};return false;}
                    placed.push_back({std::move(payload),frame,original.origin_datum[3]});
                }
                if(placed.size()==before){error={ErrorCode::InvalidInput,"periodic placement has no face on its module plane"};return false;}
            }
            std::shared_ptr<const vt::VtPartSnapshot> prepared;
            if(!vt::vt_make_periodic_material(domain,placed,module.base_program,result->material,prepared,message)) {
                error={ErrorCode::InvalidInput,message};return false;
            }
            result->modules.push_back(std::move(prepared));
        }
        if(!result->modules.empty()) {
            uint64_t identity=vt::vt_finite_hash_word(14695981039346656037ull,0x4d415050494e4701ull);
            for(const auto& module:result->modules)identity=vt::vt_finite_hash_word(identity,module->context.variant_hash);
            for(const auto& mapping:recipe.material_mappings) {
                MaterialMapping m;m.module=mapping.module;m.frame=frame_of(mapping.frame);
                m.phase=mapping.phase;m.u_range_m=mapping.u_range_m;m.datum_m=mapping.datum_m;
                result->material_mappings.push_back(m);
                identity=vt::vt_finite_hash_word(identity,m.module);
                const auto add=[&](float f){uint32_t bits;std::memcpy(&bits,&f,4);identity=vt::vt_finite_hash_word(identity,bits);};
                for(const auto* row:{&mapping.frame.origin_m,&mapping.frame.u,&mapping.frame.v,&mapping.frame.n})
                    for(float f:*row)add(f);
                for(float f:m.phase)add(f);for(float f:m.u_range_m)add(f);add(m.datum_m);
            }
            result->periodic_hash=identity;
        }
        if(!current()) return false;
        stats.bytes=result->sources->bytes();out=std::move(result);return true;
    }
    const auto directory=std::filesystem::path(root)/"finite-surfaces";
    std::error_code io_error;
    std::filesystem::create_directories(directory,io_error);
    if (io_error) {error={ErrorCode::Unavailable,"finite source cache directory: "+io_error.message()};return false;}
    auto result=std::make_shared<Prepared>();
    result->part_hash=recipe.geometry.resolved_hash;
    result->base_hash=recipe.base_hash; result->base_program=recipe.base_program;
    result->material=recipe.geometry.source.material;
    std::vector<vt::VtFiniteSourceBinding> bindings;
    for (const auto& face:faces) {
        if (!current()) return false;
        const uint64_t cache_key=vt::vt_finite_hash_word(face_recipe_digest(face.geometry),recipe.appearance_hash);
        if(source_cache) {
            auto it=source_cache->faces.find(cache_key);
            if(it!=source_cache->faces.end()) if(auto stamp=it->second.lock()) {
                bindings.push_back({std::move(stamp),face.geometry.frame,face.datum_m});
                ++stats.material_hits;++stats.faces;continue;
            }
        }
        char name[32]; std::snprintf(name,sizeof(name),"%016llx.pfac",
            static_cast<unsigned long long>(face_recipe_digest(face.geometry)));
        FacePatch geometry; FaceCacheStats cached;
        const auto path=(directory/name).string();
        if (!load_or_project_face(path,face.geometry,project,geometry,cached,error,control)) return false;
        stats.geometry_hits+=cached.hit ? 1u : 0u;
        FaceMaterialJob material{face.geometry,&geometry,recipe.appearance_program};
        FaceMaterialPatch pixels; FaceStats timing;
        if (!shade(material,pixels,timing,error,control)) return false;
        std::shared_ptr<const surface_stamp::Stamp> stamp;
        if (!surface_stamp::prepare_projected(material,pixels,stamp,error,control)) {
            error.message="face "+std::to_string(stats.faces)+": "+error.message;return false;
        }
        if(source_cache) {
            for(auto it=source_cache->faces.begin();it!=source_cache->faces.end();)
                if(it->second.expired()) it=source_cache->faces.erase(it);else ++it;
            if(source_cache->faces.size()<4096) source_cache->faces[cache_key]=stamp;
        }
        bindings.push_back({std::move(stamp),face.geometry.frame,face.datum_m}); ++stats.faces;
    }
    std::string message;
    if (!vt::vt_make_finite_sources(bindings,result->sources,message)) {
        error={ErrorCode::InvalidInput,message}; return false;
    }
    if (!current()) return false;
    stats.bytes=result->sources->bytes(); out=std::move(result); return true;
}
bool bind(const Prepared& prepared,vt::VtPartContext& ctx,BindingScratch& scratch,std::string& error) {
    const auto fail=[&](const char* text){error=text;return false;};
    if (prepared.base_program.empty() || !ctx.positions || !ctx.normals || !ctx.vertex_count ||
        (prepared.kind==Prepared::Kind::Finite && !prepared.sources))
        return fail("part surface binding requires a complete source and receiver geometry");
    BindingScratch candidate;
    candidate.weights.assign(ctx.vertex_count,255);
    if (prepared.kind==Prepared::Kind::Finite) {
    candidate.ids.resize(ctx.vertex_count);
    for (uint32_t v=0;v<ctx.vertex_count;++v) {
        const float* p=ctx.positions+size_t(v)*3;
        const float* n=ctx.normals+size_t(v)*3;
        const auto& receiver_faces=prepared.sources->receivers.empty()?prepared.sources->bindings:prepared.sources->receivers;
        for (size_t b=0;b<receiver_faces.size();++b) {
            const auto& f=receiver_faces[b];
            float plane=0,facing=0;
            for (unsigned k=0;k<3;++k) {plane+=(p[k]-f.origin_datum[k])*f.n[k];facing+=n[k]*f.n[k];}
            if (!std::isfinite(plane)||!std::isfinite(facing)||std::abs(plane-f.origin_datum[3])>f.u[3]||facing<f.n[3]) continue;
            if (candidate.ids[v]) return fail("receiver vertex matches multiple finite source faces");
            candidate.ids[v]=uint32_t(b+1);
        }
        if (!candidate.ids[v]) return fail("receiver vertex does not match a finite source face");
    }
    for (uint32_t t=0;t<ctx.triangle_count;++t) {
        if (!ctx.indices) return fail("finite receiver requires indexed triangles");
        const auto* tri=ctx.indices+size_t(t)*3;
        if (tri[0]>=ctx.vertex_count||tri[1]>=ctx.vertex_count||tri[2]>=ctx.vertex_count||
            candidate.ids[tri[0]]!=candidate.ids[tri[1]]||candidate.ids[tri[0]]!=candidate.ids[tri[2]])
            return fail("finite receiver triangle crosses source planes");
    }
    }
    scratch=std::move(candidate);
    ctx.finite_sources=prepared.sources;
    ctx.finite_source_ids=scratch.ids.empty()?nullptr:scratch.ids.data();
    ctx.surface_tape_text=prepared.base_program.c_str(); ctx.surface_tape_hash=prepared.base_hash;
    ctx.surface_materials=&prepared.material; ctx.surface_material_count=1;
    ctx.surface_weights=scratch.weights.data(); ctx.surface_world_anchored=0;
    ctx.surface_lanes=nullptr; ctx.surface_lane_count=0;
    error.clear(); return true;
}
}
