#include <matter/asset/settling.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <mutex>
#include <set>

#if MATTER_PHYSICS_BOX3D
#include "tileset_settle.h"
#include <box3d/box3d.h>
#include <box3d/collision.h>
#include "solver_identity.h"
#endif

namespace matter::asset::collision {
bool generation_available() noexcept { return MATTER_PHYSICS_BOX3D != 0; }
#if MATTER_PHYSICS_BOX3D
namespace {
using E=data::ErrorCode;
using V=data::Value;
using A=V::Array;
using O=V::Object;
std::timed_mutex solver_mutex;
bool finite(data::Vec3 p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool unit(data::Quat q) { double n=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w; return std::isfinite(n)&&std::abs(n-1)<1e-4; }
bool identity(data::Quat q) { return q.x==0 && q.y==0 && q.z==0 && q.w==1; }
bool bounded(data::Vec3 p, float limit) { return finite(p)&&std::abs(p.x)<=limit&&std::abs(p.y)<=limit&&std::abs(p.z)<=limit; }
tileset::Pose native(const Pose& p) { return {p.position.x,p.position.y,p.position.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w}; }
V vec(data::Vec3 p) { return A{p.x,p.y,p.z}; }
V pose(const Pose& p) { return O{{"position",vec(p.position)},{"rotation",A{p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w}}}; }
V settings_value(const Settings& s) {
    return O{{"dt_seconds",s.dt},{"substeps",s.substeps},{"max_sim_time_seconds",s.max_sim_time},
        {"sleep_fraction",s.sleep_fraction},{"sim_scale",s.sim_scale},{"micro_relax_steps",s.micro_relax_steps},
        {"finalize",s.finalize},{"gravity_m_s2",A{0,-9.8f,0}}};
}
V request_value(const Request& r) {
    A heights,groups,layers,caps;
    for (float h:r.ground.heights) heights.emplace_back(h);
    for (const auto& g:r.sync_groups) { A frames; for (const auto& p:g) frames.push_back(pose(p)); groups.emplace_back(std::move(frames)); }
    for (const auto& layer:r.layers) {
        A spawns;
        for (const auto& s:layer) spawns.emplace_back(O{{"body",s.body.value},{"velocity",vec(s.velocity)},{"sync_group",int(s.sync_group)},{"occurrence",s.occurrence}});
        layers.emplace_back(std::move(spawns));
    }
    for (const auto& c:r.required_capabilities) caps.emplace_back(O{{"name",c.name},{"version",c.version}});
    return O{{"collision",to_value(r.collision)},{"torus_size",r.torus_size},{"settings",settings_value(r.settings)},
        {"ground",O{{"count_x",r.ground.count_x},{"count_z",r.ground.count_z},{"cell",r.ground.cell},{"heights",std::move(heights)}}},
        {"sync_groups",std::move(groups)},{"layers",std::move(layers)},{"required_capabilities",std::move(caps)}};
}
tileset::ColliderFit fit(const Shape& s) {
    tileset::ColliderFit f;
    f.center[0]=s.local.position.x; f.center[1]=s.local.position.y; f.center[2]=s.local.position.z;
    auto q=s.local.rotation;
    // Rotation columns are the legacy collider's local axes.
    f.axis[0][0]=1-2*(q.y*q.y+q.z*q.z); f.axis[0][1]=2*(q.x*q.y+q.z*q.w); f.axis[0][2]=2*(q.x*q.z-q.y*q.w);
    f.axis[1][0]=2*(q.x*q.y-q.z*q.w); f.axis[1][1]=1-2*(q.x*q.x+q.z*q.z); f.axis[1][2]=2*(q.y*q.z+q.x*q.w);
    f.axis[2][0]=2*(q.x*q.z+q.y*q.w); f.axis[2][1]=2*(q.y*q.z-q.x*q.w); f.axis[2][2]=1-2*(q.x*q.x+q.y*q.y);
    f.half_extent[0]=s.half_extents.x; f.half_extent[1]=s.half_extents.y; f.half_extent[2]=s.half_extents.z;
    f.radius=s.radius; f.seg_half=s.half_segment;
    if (s.kind=="box") f.type=tileset::ColliderType::Box;
    else if (s.kind=="sphere") f.type=tileset::ColliderType::Sphere;
    else if (s.kind=="capsule") f.type=tileset::ColliderType::Capsule;
    else {
        f.type=tileset::ColliderType::Hull;
        for (auto p:s.vertices) for (int k=0;k<3;++k) {
            float c=k==0?s.local.position.x:k==1?s.local.position.y:s.local.position.z;
            f.hull_points.push_back(c+f.axis[0][k]*p.x+f.axis[1][k]*p.y+f.axis[2][k]*p.z);
        }
    }
    return f;
}
}
#endif
SettleResult settle(const services::JobEnvelope& envelope, const Request& r,
                    const services::ResourceBudget& budget, const services::JobControl& control) {
#if !MATTER_PHYSICS_BOX3D
    (void)r; (void)budget; (void)control;
    return SettleResult::failure(envelope,data::ErrorCode::unsupported_capability,"Box3d generation backend was not built");
#else
    auto fail=[&](E e,const char* m){return SettleResult::failure(envelope,e,m);};
    try {
        services::JobMonitor monitor(budget,control);
        auto v=services::validate(envelope); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        v=monitor.checkpoint(); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        v=services::require_capabilities(r.required_capabilities,{{"matter.physics.settle.box3d",1}});
        if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        data::ValidationLimits limits;
        limits.max_vertices=budget.vertices; limits.max_indices=budget.indices;
        limits.max_document_bytes=std::min(limits.max_document_bytes,budget.working_bytes);
        v=validate(r.collision,limits); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        const auto& s=r.settings;
        // Bounded operating envelope prevents the legacy toroidal while loops
        // and float time accumulator from stalling on extreme valid floats.
        if (!std::isfinite(r.torus_size)||r.torus_size<0.01f||r.torus_size>10000 ||
            !std::isfinite(s.dt)||s.dt<1e-5f||s.dt>0.1f||s.substeps<1||s.substeps>64 ||
            !std::isfinite(s.max_sim_time)||s.max_sim_time<=0||s.max_sim_time>600 ||
            !std::isfinite(s.sleep_fraction)||s.sleep_fraction<0||s.sleep_fraction>1 ||
            !std::isfinite(s.sim_scale)||s.sim_scale<0.01f||s.sim_scale>100 ||s.micro_relax_steps>1024)
            return fail(E::invalid_input,"settings outside supported settling envelope");
        const auto& h=r.ground;
        const uint64_t samples=uint64_t(h.count_x)*h.count_z;
        if (samples>budget.vertices || h.heights.size()>budget.vertices || r.layers.size()>10000 || r.sync_groups.size()>10000)
            return fail(E::resource_limit,"settle input count budget");
        if (h.count_x<2||h.count_z<2||h.count_x>4097||h.count_z>4097 || samples!=h.heights.size() || !std::isfinite(h.cell)||h.cell<=0 ||
            std::abs(double(h.count_x-1)*h.cell-r.torus_size)>1e-4*r.torus_size ||
            std::abs(double(h.count_z-1)*h.cell-r.torus_size)>1e-4*r.torus_size)
            return fail(E::invalid_input,"heightfield must cover the torus with complete samples");
        for (float height:h.heights) if (!std::isfinite(height)||std::abs(height)>10000) return fail(E::invalid_input,"invalid heightfield sample");
        if (!r.collision.joints.empty()) return fail(E::unsupported_capability,"settling backend does not simulate joints");
        std::map<std::string,size_t> body_indices,shape_indices;
        for (size_t i=0;i<r.collision.bodies.size();++i) {
            const auto& b=r.collision.bodies[i]; body_indices.emplace(b.id.value,i);
            if (b.motion!="dynamic"||b.restitution!=0) return fail(E::unsupported_capability,"settling accepts dynamic bodies with zero restitution");
            if (!bounded(b.pose.position,10000)||b.pose.position.x<0||b.pose.position.x>=r.torus_size||b.pose.position.z<0||b.pose.position.z>=r.torus_size ||
                b.density>1e6f||b.friction>10) return fail(E::invalid_input,"body outside settling envelope");
        }
        std::vector<tileset::ColliderFit> fits;
        for (const auto& shape:r.collision.shapes) {
            if (shape.kind=="triangle_mesh") return fail(E::unsupported_capability,"dynamic triangle mesh settling is unsupported");
            if (shape.vertices.size()>64) return fail(E::unsupported_capability,"settling convex hull budget is 64 points");
            if (!bounded(shape.local.position,1000)||!bounded(shape.half_extents,1000)||shape.radius>1000||shape.half_segment>1000)
                return fail(E::invalid_input,"shape outside settling envelope");
            if (shape.radius*s.sim_scale<1e-6f || shape.half_extents.x*s.sim_scale<1e-6f ||
                shape.half_extents.y*s.sim_scale<1e-6f || shape.half_extents.z*s.sim_scale<1e-6f)
                return fail(E::invalid_input,"shape dimensions below supported simulation precision");
            for (auto p:shape.vertices) if (!bounded(p,1000)) return fail(E::invalid_input,"hull outside settling envelope");
            shape_indices.emplace(shape.id.value,fits.size()); fits.push_back(fit(shape));
        }
        std::vector<std::vector<size_t>> group_counts;
        uint64_t frame_count=0;
        for (const auto& group:r.sync_groups) {
            if (group.empty()||group.size()>1024) return fail(E::invalid_input,"invalid sync group size");
            frame_count+=group.size();
            if (frame_count>limits.max_instances) return fail(E::resource_limit,"sync frame budget");
            group_counts.emplace_back(group.size(),0);
            for (const auto& p:group) {
                if (!bounded(p.position,r.torus_size)||!unit(p.rotation)) return fail(E::invalid_input,"invalid sync occurrence frame");
                // Legacy sync only subtracts translations; do not pretend it
                // supports rotated occurrence frames.
                if (!identity(p.rotation)) return fail(E::unsupported_capability,"rotated sync frames are unsupported by the legacy solver");
            }
        }
        std::set<std::string> seen;
        std::vector<size_t> spawn_order;
        for (const auto& layer:r.layers) {
            for (const auto& spawn:layer) {
                auto it=body_indices.find(spawn.body.value);
                if (it==body_indices.end()||!seen.insert(spawn.body.value).second||!bounded(spawn.velocity,100)) return fail(E::invalid_input,"invalid or duplicate spawn");
                spawn_order.push_back(it->second);
                if (spawn.sync_group < -1 || (spawn.sync_group>=0 && (size_t(spawn.sync_group)>=group_counts.size() || spawn.occurrence>=group_counts[size_t(spawn.sync_group)].size())) ||
                    (spawn.sync_group==-1 && spawn.occurrence!=0)) return fail(E::invalid_input,"invalid sync binding");
                if (spawn.sync_group>=0) ++group_counts[size_t(spawn.sync_group)][spawn.occurrence];
            }
            for (const auto& counts:group_counts) for (auto c:counts) if (c!=counts.front()) return fail(E::invalid_input,"sync occurrences must have matching member counts after each layer");
        }
        if (seen.size()!=r.collision.bodies.size()) return fail(E::invalid_input,"every collision body must be spawned exactly once");
        services::ResourceUsage usage;
        usage.vertices=samples;
        for (const auto& shape:r.collision.shapes) usage.vertices+=shape.vertices.size();
        // Conservative admission estimate includes solver state and copies.
        usage.working_bytes=samples*128 + r.collision.bodies.size()*16384ull + frame_count*1024;
        v=monitor.checkpoint(usage); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        std::string input;
        v=data::canonical_json(request_value(r),input,limits); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        usage.working_bytes+=input.size()*8;
        v=monitor.checkpoint(usage); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        SettledProduct out; out.collision=r.collision;
        {
        // Lock covers hull preflight too: Box3d allocators/world slots are global.
        // try_lock checkpoints allow cancellation while another request runs.
        std::unique_lock<std::timed_mutex> lock(solver_mutex,std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(1))) {
            v=monitor.checkpoint(); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        }
        for (const auto& f:fits) if (f.type==tileset::ColliderType::Hull) {
            std::vector<b3Vec3> points;
            for (size_t i=0;i<f.hull_points.size();i+=3) points.push_back({f.hull_points[i]*s.sim_scale,f.hull_points[i+1]*s.sim_scale,f.hull_points[i+2]*s.sim_scale});
            b3HullData* hull=b3CreateHull(points.data(),static_cast<int>(points.size()),32);
            if (!hull) return fail(E::invalid_input,"Box3d rejected convex hull; sphere fallback is forbidden");
            b3DestroyHull(hull);
        }
        tileset::HeightField ground{static_cast<int>(h.count_x),static_cast<int>(h.count_z),h.cell,h.heights};
        tileset::SettleParams params;
        params.dt=s.dt; params.substeps=static_cast<int>(s.substeps); params.max_sim_time=s.max_sim_time;
        params.sleep_fraction=s.sleep_fraction; params.sim_scale=s.sim_scale; params.micro_relax_steps=static_cast<int>(s.micro_relax_steps);
        tileset::SettleWorld world(r.torus_size,ground,params);
        for (const auto& group:r.sync_groups) { std::vector<tileset::Pose> frames; for (const auto& p:group) frames.push_back(native(p)); world.add_sync_group(frames); }
        for (const auto& layer:r.layers) {
            std::vector<tileset::BodySpawn> spawns;
            for (const auto& spawn:layer) {
                const auto& b=r.collision.bodies[body_indices.at(spawn.body.value)];
                tileset::BodySpawn n;
                n.collider=&fits[shape_indices.at(b.shape.value)]; n.start=native(b.pose); n.density=b.density; n.friction=b.friction;
                n.vx=spawn.velocity.x; n.vy=spawn.velocity.y; n.vz=spawn.velocity.z; n.sync_group=spawn.sync_group; n.instance=static_cast<int>(spawn.occurrence);
                spawns.push_back(n);
            }
            world.begin_layer(spawns);
            float time=0;
            while (time<s.max_sim_time&&!world.layer_converged()) {
                usage.operations+=uint64_t(s.substeps)*std::max(1,world.body_count());
                v=monitor.checkpoint(usage); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
                const float next=world.step().sim_time;
                if (!std::isfinite(next)||next<=time) return fail(E::backend_failure,"solver time stopped advancing");
                time=next;
            }
            auto report=world.end_layer(); out.layers.push_back({report.converged,static_cast<uint32_t>(report.awake_count),report.sim_time});
        }
        if (s.finalize) {
            usage.operations+=uint64_t(s.micro_relax_steps)*s.substeps*std::max(1,world.body_count());
            v=monitor.checkpoint(usage); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
            world.finalize();
        }
        for (size_t i=0;i<spawn_order.size();++i) {
            const auto& p=world.poses()[i]; out.collision.bodies[spawn_order[i]].pose={{p.px,p.py,p.pz},{p.qx,p.qy,p.qz,p.qw}};
        }
        } // Destroy the world and release the solver lock before callbacks.
        auto version=b3GetVersion();
        out.collision.generation=O{{"solver","box3d"},{"solver_version",std::to_string(version.major)+"."+std::to_string(version.minor)+"."+std::to_string(version.revision)},
            {"solver_source_sha256",MATTER_SETTLE_SOURCE_SHA256},{"build_profile",MATTER_SETTLE_BUILD_PROFILE},
            {"backend_version",1},{"input_sha256",data::sha256(input).hex},{"settings",settings_value(s)},
            {"determinism","same-build fixture scope only; no cross-platform guarantee"}};
        std::string encoded;
        v=encode(out.collision,encoded,limits); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        usage.output_bytes=encoded.size();
        v=monitor.report({1,"settled"}); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        v=monitor.checkpoint(usage); if (!v.ok()) return SettleResult::failure(envelope,v.diagnostics);
        std::vector<data::Diagnostic> warnings;
        for (const auto& l:out.layers) if (!l.converged) { warnings.push_back({data::Severity::warning,E::backend_failure,"settling time limit reached before all layers converged",{},{},{}}); break; }
        return SettleResult::success(envelope,std::move(out),std::move(warnings));
    } catch (const std::exception& e) { return SettleResult::failure(envelope,E::backend_failure,e.what()); }
      catch (...) { return fail(E::backend_failure,"unknown settling backend failure"); }
#endif
}
} // namespace matter::asset::collision
