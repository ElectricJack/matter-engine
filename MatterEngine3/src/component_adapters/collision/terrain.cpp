#include "terrain.h"
#include "terrain_collision/terrain_collision_artifact.h"
#include <cmath>
#include <map>
#include <set>

namespace matter::component_adapters::collision {
namespace data=matter::contracts;
namespace legacy=matter::terrain_collision;
namespace {
data::ValidationResult fail(data::ErrorCode c,const char* message) { return {{{data::Severity::error,c,message,{},{},{}}}}; }
bool identity(const portable::Pose& p) {
    return p.position.x==0 && p.position.y==0 && p.position.z==0 && p.rotation.x==0 && p.rotation.y==0 && p.rotation.z==0 && p.rotation.w==1;
}
}
data::ValidationResult to_candidate(const TerrainProduct& p, legacy::TerrainCollisionCandidate& out) {
    if (p.version!=1) return fail(data::ErrorCode::unsupported_version,"terrain collision product version");
    auto v=portable::validate(p.collision); if (!v.ok()) return v;
    if (!std::isfinite(p.friction)||p.friction<0||!std::isfinite(p.restitution)||p.restitution<0||p.restitution>1)
        return fail(data::ErrorCode::invalid_input,"invalid terrain material");
    if (!p.collision.joints.empty()||p.tiles.size()!=p.collision.bodies.size()||p.tiles.size()!=p.collision.shapes.size())
        return fail(data::ErrorCode::unsupported_capability,"terrain adapter requires one independent static mesh per tile");
    std::map<std::string,const portable::Body*> bodies;
    std::map<std::string,const portable::Shape*> shapes;
    for (const auto& b:p.collision.bodies) bodies.emplace(b.id.value,&b);
    for (const auto& s:p.collision.shapes) shapes.emplace(s.id.value,&s);
    std::set<std::string> used_bodies, used_shapes;
    legacy::TerrainCollisionCandidate candidate;
    candidate.geometry_key=p.geometry_key; candidate.installation_key=p.installation_key;
    candidate.friction=p.friction; candidate.restitution=p.restitution;
    std::array<int64_t,3> previous{}; bool first=true;
    for (const auto& t:p.tiles) {
        if ((!first&&t.coordinate<=previous)||!used_bodies.insert(t.body.value).second||!bodies.count(t.body.value))
            return fail(data::ErrorCode::invalid_input,"tiles must be sorted, unique and reference unique bodies");
        previous=t.coordinate; first=false;
        const auto& b=*bodies.at(t.body.value); const auto& s=*shapes.at(b.shape.value);
        auto rotation=b.pose; rotation.position={};
        if (b.motion!="static"||b.friction!=p.friction||b.restitution!=p.restitution||!identity(rotation)||s.kind!="triangle_mesh"||!identity(s.local)||!used_shapes.insert(s.id.value).second)
            return fail(data::ErrorCode::unsupported_capability,"terrain mesh conversion would lose material, transform or shape semantics");
        legacy::TileCandidate tile;
        tile.coordinate={t.coordinate[0],t.coordinate[1],t.coordinate[2]};
        tile.origin_m={b.pose.position.x,b.pose.position.y,b.pose.position.z}; tile.tile_key=t.key; tile.digest=t.digest;
        for (auto point:s.vertices) tile.vertices.push_back({point.x,point.y,point.z});
        tile.indices=s.indices;
        candidate.stats.triangle_count+=tile.indices.size()/3; candidate.stats.unique_vertex_count+=tile.vertices.size();
        if (tile.indices.empty()) ++candidate.stats.empty_tiles;
        candidate.tiles.push_back(std::move(tile));
    }
    out=std::move(candidate); return {};
}
data::ValidationResult from_candidate(const legacy::TerrainCollisionCandidate& in, TerrainProduct& out) {
    TerrainProduct p; p.geometry_key=in.geometry_key; p.installation_key=in.installation_key;
    p.friction=in.friction; p.restitution=in.restitution;
    // Check counts before copying a potentially untrusted candidate.
    if (in.tiles.size()>data::ValidationLimits{}.max_nodes) return fail(data::ErrorCode::resource_limit,"terrain tile budget");
    uint64_t vertices=0,indices=0;
    for (const auto& t:in.tiles) {
        vertices+=t.vertices.size(); indices+=t.indices.size();
        if (vertices>data::ValidationLimits{}.max_vertices||indices>data::ValidationLimits{}.max_indices) return fail(data::ErrorCode::resource_limit,"terrain mesh budget");
        const auto id="terrain-tile-"+std::to_string(p.tiles.size());
        portable::Shape s; s.id={id+"-mesh"}; s.kind="triangle_mesh";
        for (auto point:t.vertices) s.vertices.push_back({point.x,point.y,point.z});
        s.indices=t.indices;
        portable::Body b; b.id={id}; b.shape=s.id; b.motion="static";
        b.pose.position={t.origin_m.x,t.origin_m.y,t.origin_m.z}; b.friction=in.friction; b.restitution=in.restitution;
        p.tiles.push_back({{t.coordinate.x,t.coordinate.y,t.coordinate.z},t.tile_key,t.digest,b.id});
        p.collision.shapes.push_back(std::move(s)); p.collision.bodies.push_back(std::move(b));
    }
    legacy::TerrainCollisionCandidate check;
    auto v=to_candidate(p,check); if (v.ok()) out=std::move(p); return v;
}
} // namespace matter::component_adapters::collision
