#include "terrain.h"
#include "terrain_collision/terrain_collision_artifact.h"
#include <iostream>
#include <stdexcept>
namespace adapter=matter::component_adapters::collision;
namespace legacy=matter::terrain_collision;
void check(bool ok,const char* m) { if (!ok) throw std::runtime_error(m); }
int main() {
    try {
        legacy::TerrainCollisionCandidate original;
        original.geometry_key=UINT64_MAX; original.installation_key=UINT64_MAX-1; original.friction=0.8f; original.restitution=0.25f;
        legacy::TileCandidate tile; tile.coordinate={-9007199254740993LL,2,3}; tile.origin_m={100,2,-100}; tile.tile_key=UINT64_MAX-2; tile.digest=UINT64_MAX-3;
        tile.vertices={{0,0,0},{0,0,1},{1,0,0}}; tile.indices={0,1,2}; original.tiles.push_back(tile);
        tile.coordinate={9007199254740993LL,2,3}; tile.vertices.clear(); tile.indices.clear(); original.tiles.push_back(tile);
        adapter::TerrainProduct product; check(adapter::from_candidate(original,product).ok(),"convert terrain");
        legacy::TerrainCollisionCandidate back; check(adapter::to_candidate(product,back).ok(),"return to existing terrain consumer");
        check(back.geometry_key==original.geometry_key&&back.installation_key==original.installation_key,"64-bit generation keys");
        check(back.friction==original.friction&&back.restitution==original.restitution,"terrain material");
        check(back.tiles.size()==2&&back.tiles[1].vertices.empty()&&back.stats.empty_tiles==1,"empty tiles retained");
        for (size_t i=0;i<back.tiles.size();++i) {
            const auto& a=original.tiles[i]; const auto& b=back.tiles[i];
            check(a.coordinate==b.coordinate&&a.tile_key==b.tile_key&&a.digest==b.digest,"exact tile identities");
            check(a.origin_m.x==b.origin_m.x&&a.origin_m.y==b.origin_m.y&&a.origin_m.z==b.origin_m.z&&a.indices==b.indices,"tile placement and indices");
            for (size_t j=0;j<a.vertices.size();++j) check(a.vertices[j].x==b.vertices[j].x&&a.vertices[j].y==b.vertices[j].y&&a.vertices[j].z==b.vertices[j].z,"exact local mesh");
        }
        product.collision.bodies[0].motion="dynamic";
        check(!adapter::to_candidate(product,back).ok()&&back.tiles.size()==2,"failed conversion preserves consumer");
        original.tiles[0].indices[0]=99; check(!adapter::from_candidate(original,product).ok(),"invalid candidate rejected");
        std::cout<<"PHYS-TERRAIN exact collision consumer round trip pass\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
