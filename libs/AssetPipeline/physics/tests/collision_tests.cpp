#include <matter/asset/collision.h>
#include <matter/asset/settling.h>
#include <matter/asset/archive.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#if MATTER_PHYSICS_BOX3D
#include "tileset_settle.h"
#endif
using namespace matter::asset::collision;
namespace {
services::JobEnvelope job{"physics-fixture","v1",17};
void check(bool ok,const char* m) { if (!ok) throw std::runtime_error(m); }
void valid(const data::ValidationResult& v) { if (!v.ok()) throw std::runtime_error(v.diagnostics.front().message); }
template<class T> void success(const T& r) { if (!r.ok()) throw std::runtime_error(r.diagnostics().front().message); }
template<class T> void failure(const T& r,data::ErrorCode code) { check(!r.ok()&&!r.value()&&r.diagnostics().front().code==code,"expected failure without product"); }
Product boxes() {
    Product p; p.required_capabilities={{capability,1}};
    Shape s; s.id={"shared-box"}; s.half_extents={0.05f,0.05f,0.05f}; p.shapes.push_back(s);
    data::Rng rng(777);
    for (int i=0;i<30;++i) {
        Body b; b.id={"box-"+std::to_string(i)}; b.shape=s.id;
        b.pose.position={float(rng.next_unit())*8,0.3f+0.5f*float(rng.next_unit()),float(rng.next_unit())*8};
        p.bodies.push_back(b);
    }
    return p;
}
Request request() {
    Request r; r.collision=boxes(); r.ground={33,33,0.25f,std::vector<float>(33*33,0)};
    r.settings.finalize=false;
    r.layers.emplace_back(); for (const auto& b:r.collision.bodies) r.layers.back().push_back({b.id,{}});
    r.required_capabilities={{"matter.physics.settle.box3d",1}}; return r;
}
services::ResourceBudget budget() { services::ResourceBudget b; b.operations=10000000; b.execution_microseconds=30000000; return b; }
void products() {
    auto p=boxes();
    Joint j; j.id={"tie"}; j.body_a=p.bodies[0].id; j.body_b=p.bodies[1].id; j.distance=0.3f; p.joints.push_back(j);
    auto weld=j; weld.id={"weld"}; weld.kind="weld"; weld.distance=0; p.joints.push_back(weld);
    for (const char* kind:{"sphere","capsule","convex_hull","triangle_mesh"}) {
        Shape s; s.id={kind}; s.kind=kind;
        if (s.kind=="convex_hull") s.vertices={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
        if (s.kind=="triangle_mesh") { s.vertices={{0,0,0},{1,0,0},{0,1,0}}; s.indices={0,1,2}; }
        p.shapes.push_back(s);
    }
    std::string bytes,again; valid(encode(p,bytes)); Product decoded; valid(decode(bytes,decoded)); valid(encode(decoded,again));
    check(bytes==again,"PHYS-PRODUCT: exact product round trip");
    valid(decode(extension(p),decoded)); valid(encode(decoded,again)); check(bytes==again,"extension round trip");
    auto bad=p; bad.version=2; check(validate(bad).diagnostics[0].code==data::ErrorCode::unsupported_version,"product version");
    bad=p; bad.required_capabilities.push_back({"unknown",1}); check(!validate(bad).ok(),"unknown required capability");
    bad=p; bad.shapes[0].kind="cone"; check(validate(bad).diagnostics[0].code==data::ErrorCode::unsupported_capability,"shape diagnostic");
    bad=p; bad.joints[0].kind="revolute"; check(validate(bad).diagnostics[0].code==data::ErrorCode::unsupported_capability,"joint diagnostic");
    bad=p; bad.bodies[0].pose.rotation={0,0,0,0}; check(!validate(bad).ok(),"bad quaternion");
    bad=p; bad.shapes.back().indices[2]=99; check(!validate(bad).ok(),"bad triangle index");
    bad=p; bad.shapes.back().vertices[2]=bad.shapes.back().vertices[1]; check(!validate(bad).ok(),"degenerate triangle");
    bad=p; bad.shapes[0].radius=std::numeric_limits<float>::infinity(); check(!validate(bad).ok(),"nonfinite dimension");
    bad=p; bad.joints[0].body_a={"absent"}; check(!validate(bad).ok(),"missing joint body");
    data::ValidationLimits limits; limits.max_instances=1; check(!validate(p,limits).ok(),"object budget");
    check(!decode("{}",decoded).ok(),"malformed document"); valid(encode(decoded,again)); check(bytes==again,"failed decode must preserve destination");
    check(!convert_solver("box3d","physx").ok(),"cross solver conversion diagnostic");
    auto ext=extension(p); ext.capability.version=2; check(!decode(ext,decoded).ok(),"extension version");
    std::cout<<"PHYS-PRODUCT / PHYS-UNSUPPORTED pass\n";
}
matter::asset::Bytes archive(const Product& p) {
    data::AssetDocument d; d.id={"baked-placements"}; d.provenance.id={"PHYS-BAKED"}; d.provenance.uri="fixture:physics"; d.provenance.revision="1";
    d.required_capabilities={{"matter.asset.core",1},{"matter.material.pbr",1},{capability,1}};
    d.extensions={extension(p)};
    data::MaterialDescription material; material.id={"plain"}; d.materials.push_back(material);
    data::MeshPrototype mesh; mesh.id={"triangle"}; mesh.positions={{0,0,0},{1,0,0},{0,1,0}}; mesh.indices={0,1,2}; mesh.bounds={{0,0,0},{1,1,0}}; mesh.submeshes={{0,3,material.id}};
    d.meshes={mesh}; d.bounds=mesh.bounds; data::Instance instance; instance.id={"triangle-instance"}; instance.prototype=mesh.id; d.instances={instance};
    services::EvaluationOptions options; options.validation.supported_capabilities.push_back({capability,1});
    auto out=matter::asset::encode_prepared(job,services::AssetProduct{d},options); success(out); return *out.value();
}
void read_archive(const matter::asset::Bytes& bytes) {
    services::EvaluationOptions options; options.validation.supported_capabilities.push_back({capability,1});
    auto out=matter::asset::decode_prepared(job,bytes,options); success(out);
    Product p; valid(decode(out.value()->document.extensions[0],p));
    check(p.bodies.size()==30,"baked shared collision geometry");
    for (const auto& b:p.bodies) check(b.pose.position.y>0.03f&&b.pose.position.y<0.2f,"PHYS-BAKED: settled heights loaded without a solver");
    const auto& provenance=std::get<data::Value::Object>(p.generation.data);
    check(std::get<std::string>(provenance.at("solver").data)=="box3d","solver provenance");
    check(std::get<std::string>(provenance.at("solver_source_sha256").data).size()==64,"solver source digest");
    check(std::get<std::string>(provenance.at("input_sha256").data).size()==64,"input digest");
    std::cout<<"PHYS-BAKED pass; generation_available="<<generation_available()<<"\n";
}
#if MATTER_PHYSICS_BOX3D
void compare(const Request& r, const SettledProduct& result) {
    tileset::HeightField ground{int(r.ground.count_x),int(r.ground.count_z),r.ground.cell,r.ground.heights};
    tileset::SettleParams settings;
    settings.dt=r.settings.dt; settings.substeps=int(r.settings.substeps); settings.max_sim_time=r.settings.max_sim_time;
    settings.sleep_fraction=r.settings.sleep_fraction; settings.sim_scale=r.settings.sim_scale; settings.micro_relax_steps=int(r.settings.micro_relax_steps);
    tileset::SettleWorld legacy(r.torus_size,ground,settings);
    for (const auto& group:r.sync_groups) { std::vector<tileset::Pose> frames; for (const auto& p:group) frames.push_back({p.position.x,p.position.y,p.position.z,0,0,0,1}); legacy.add_sync_group(frames); }
    tileset::ColliderFit box; box.type=tileset::ColliderType::Box; box.half_extent[0]=box.half_extent[1]=box.half_extent[2]=0.05f;
    std::vector<std::string> order;
    for (const auto& layer:r.layers) {
        std::vector<tileset::BodySpawn> spawns;
        for (const auto& s:layer) {
            const Body* body=nullptr; for (const auto& b:r.collision.bodies) if (b.id==s.body) body=&b;
            check(body!=nullptr,"oracle body"); tileset::BodySpawn n; n.collider=&box;
            const auto& p=body->pose; n.start={p.position.x,p.position.y,p.position.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w};
            n.density=body->density; n.friction=body->friction; n.vx=s.velocity.x; n.vy=s.velocity.y; n.vz=s.velocity.z;
            n.sync_group=s.sync_group; n.instance=int(s.occurrence); spawns.push_back(n); order.push_back(s.body.value);
        }
        legacy.settle_layer(spawns);
    }
    if (r.settings.finalize) legacy.finalize();
    for (size_t i=0;i<order.size();++i) {
        const auto& p=legacy.poses()[i]; const Body* body=nullptr;
        for (const auto& b:result.collision.bodies) if (b.id.value==order[i]) body=&b;
        check(body!=nullptr,"result body"); const auto& q=body->pose;
        check(p.px==q.position.x&&p.py==q.position.y&&p.pz==q.position.z&&p.qx==q.rotation.x&&p.qy==q.rotation.y&&p.qz==q.rotation.z&&p.qw==q.rotation.w,"PHYS-PARITY: exact same-build legacy placement");
    }
}
#endif
void generation() {
    auto r=request(); auto b=budget();
#if MATTER_PHYSICS_BOX3D
    check(generation_available(),"backend available"); auto out=settle(job,r,b); success(out);
    check(out.envelope()==job&&out.value()->layers[0].converged,"converged with echoed envelope"); compare(r,*out.value()); read_archive(archive(out.value()->collision));
    auto repeat=settle(job,r,b); success(repeat); std::string a,c; valid(encode(out.value()->collision,a)); valid(encode(repeat.value()->collision,c)); check(a==c,"same build repeatability");
    // Two layers with a portal-synchronized pair and finalize; spawn order is
    // deliberately different from document order, preserving instance IDs.
    r.layers={{{r.collision.bodies[1].id,{},0,0},{r.collision.bodies[0].id,{},0,1}}, {}};
    r.collision.bodies[1].pose.position={1,0.5f,2}; r.collision.bodies[0].pose.position={5,0.5f,2};
    r.sync_groups={{{{0,0,0},{0,0,0,1}},{{4,0,0},{0,0,0,1}}}}; r.settings.finalize=true;
    for (size_t i=2;i<r.collision.bodies.size();++i) r.layers[1].push_back({r.collision.bodies[i].id,{}});
    out=settle(job,r,b); success(out); compare(r,*out.value());
    check(std::abs((out.value()->collision.bodies[0].pose.position.x-4)-out.value()->collision.bodies[1].pose.position.x)<1e-5,"portal shared placement tolerance");
    // Representative supported primitives and convex hull on the same ground.
    auto shapes=request(); shapes.collision.bodies.resize(4); shapes.layers[0].resize(4); shapes.collision.shapes.clear();
    const char* kinds[]={"box","sphere","capsule","convex_hull"};
    for (int i=0;i<4;++i) {
        Shape shape; shape.id={kinds[i]}; shape.kind=kinds[i]; shape.radius=0.05f; shape.half_extents={0.05f,0.05f,0.05f}; shape.half_segment=0.1f;
        if (shape.kind=="convex_hull") for (float x:{-0.05f,0.05f}) for (float y:{-0.05f,0.05f}) for (float z:{-0.05f,0.05f}) shape.vertices.push_back({x,y,z});
        shapes.collision.shapes.push_back(shape); shapes.collision.bodies[i].shape=shape.id; shapes.collision.bodies[i].pose.position={1.0f+float(i),0.5f,2};
    }
    auto shape_result=settle(job,shapes,b); success(shape_result);
    for (const auto& body:shape_result.value()->collision.bodies) check(std::abs(body.pose.position.y-0.05f)<0.02f,"PHYS-SHAPES: resting height tolerance 0.02 metres");
    auto wrap=shapes; wrap.collision.bodies.resize(1); wrap.collision.shapes={shapes.collision.shapes[1]}; wrap.collision.bodies[0].shape=wrap.collision.shapes[0].id;
    wrap.collision.bodies[0].pose.position={7.9f,1,2}; wrap.layers[0].resize(1); wrap.layers[0][0].velocity={4,0,0}; wrap.settings.max_sim_time=0.1f;
    auto wrapped=settle(job,wrap,b); success(wrapped); check(wrapped.value()->collision.bodies[0].pose.position.x<1,"PHYS-WRAP: crosses positive torus edge");
    auto invalid=request(); invalid.settings.dt=0; failure(settle(job,invalid,b),data::ErrorCode::invalid_input);
    invalid=request(); invalid.required_capabilities.push_back({"unknown",1}); failure(settle(job,invalid,b),data::ErrorCode::unsupported_capability);
    invalid=request(); invalid.ground.heights.pop_back(); failure(settle(job,invalid,b),data::ErrorCode::invalid_input);
    invalid=request(); invalid.layers[0][0].sync_group=99; failure(settle(job,invalid,b),data::ErrorCode::invalid_input);
    invalid=request(); invalid.layers[0].push_back(invalid.layers[0][0]); failure(settle(job,invalid,b),data::ErrorCode::invalid_input);
    invalid=request(); invalid.collision.shapes[0].kind="convex_hull"; invalid.collision.shapes[0].vertices={{0,0,0},{1,0,0},{0,0,1},{1,0,1}};
    failure(settle(job,invalid,b),data::ErrorCode::invalid_input);
    invalid=request(); Joint j; j.id={"joint"}; j.body_a=invalid.collision.bodies[0].id; j.body_b=invalid.collision.bodies[1].id; invalid.collision.joints.push_back(j);
    failure(settle(job,invalid,b),data::ErrorCode::unsupported_capability);
    services::CancellationSource cancel; cancel.cancel(); services::JobControl control; control.cancellation=cancel.token();
    failure(settle(job,request(),b,control),data::ErrorCode::cancelled);
    auto small=b; small.operations=1; failure(settle(job,request(),small),data::ErrorCode::resource_limit);
    control={}; services::CancellationSource late; control.cancellation=late.token(); control.progress=[&](const services::Progress&){late.cancel();};
    failure(settle(job,request(),b,control),data::ErrorCode::cancelled);
    control={}; control.progress=[](const services::Progress&){throw std::runtime_error("callback failed");}; failure(settle(job,request(),b,control),data::ErrorCode::backend_failure);
    std::cout<<"PHYS-PARITY / PHYS-PORTAL / PHYS-FAILURE pass\n";
#else
    check(!generation_available(),"backend disabled"); failure(settle(job,r,b),data::ErrorCode::unsupported_capability);
    std::cout<<"PHYS-SOLVER-OFF pass\n";
#endif
}
}
int main(int argc,char** argv) {
    try {
        check(argc>=2,"test mode required"); std::string mode=argv[1];
        if (mode=="products") products();
        else if (mode=="generation") generation();
        else if (mode=="write") {
            check(argc==3,"output path required"); auto r=settle(job,request(),budget()); success(r); auto bytes=archive(r.value()->collision);
            std::ofstream f(argv[2],std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); f.close(); check(bool(f),"write fixture");
        } else if (mode=="read") {
            check(argc==3,"input path required"); std::ifstream f(argv[2],std::ios::binary); check(bool(f),"read fixture"); matter::asset::Bytes bytes{std::istreambuf_iterator<char>(f),{}}; read_archive(bytes);
        } else check(false,"unknown fixture");
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
