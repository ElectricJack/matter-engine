#pragma once
// Bounded binary interchange for native bake/render diagnostics. This is not
// a production cache ABI. Scalars are IEEE floats and little-endian integers;
// the endian tag rejects readers with a different host byte order.
#include "sparse_voxel_hierarchy.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace sparse_hierarchy_fixture {
struct NodeRecord {
    uint64_t key,geometry_key,expanded;
    float bounds[6];
    uint32_t valid,depth,children,levels;
};
struct AssetRecord { float grid[4]; uint32_t bricks,cells; };
struct BrickRecord { int32_t coord[3]; uint32_t mask[2],first; };
struct CellRecord { double values[32]; uint32_t flags,reserved; };
struct ChildRecord { float transform[16]; uint32_t node; };
static_assert(sizeof(NodeRecord)==64 && sizeof(AssetRecord)==24 && sizeof(BrickRecord)==24);
static_assert(sizeof(CellRecord)==264 && sizeof(ChildRecord)==68);
template<class T> bool write(std::ostream& f,const T& value) {
    return bool(f.write(reinterpret_cast<const char*>(&value),sizeof(value)));
}
template<class T> bool read(std::istream& f,T& value) {
    return bool(f.read(reinterpret_cast<char*>(&value),sizeof(value)));
}
inline CellRecord encode(const sparse_voxel::Cell& cell) {
    CellRecord result{}; size_t i=0; result.values[i++]=cell.area;
    const auto copy=[&](const auto& values) { for(double v:values) result.values[i++]=v; };
    copy(cell.albedo_area); copy(cell.normal_area); copy(cell.normal_second_area);
    copy(cell.support_min); copy(cell.support_max); copy(cell.plane); copy(cell.projected_area);
    result.flags=(cell.has_support?1u:0u)|(cell.has_projection?2u:0u); return result;
}
inline sparse_voxel::Cell decode(const CellRecord& record) {
    sparse_voxel::Cell cell; size_t i=0; cell.area=record.values[i++];
    const auto copy=[&](auto& values) { for(double& v:values) v=record.values[i++]; };
    copy(cell.albedo_area); copy(cell.normal_area); copy(cell.normal_second_area);
    copy(cell.support_min); copy(cell.support_max); copy(cell.plane); copy(cell.projected_area);
    cell.has_support=(record.flags&1)!=0; cell.has_projection=(record.flags&2)!=0; return cell;
}
inline bool write(std::ostream& f,const sparse_voxel::Hierarchy& hierarchy,std::string& error) {
    error.clear();
    const uint64_t magic=0x3147414944485653ull; // SVHDIAG1
    const uint32_t endian=0x01020304u,nodes=uint32_t(hierarchy.prototypes.size()),roots=uint32_t(hierarchy.roots.size());
    const auto& s=hierarchy.stats;
    const uint64_t stats[]={s.source_triangles,s.child_links,s.cell_tests,s.stored_cells,
        s.generated_nodes,s.compiled_child_links,s.max_children,s.max_depth};
    write(f,magic); write(f,endian); write(f,nodes); write(f,roots); write(f,stats);
    for(uint32_t root:hierarchy.roots) write(f,root);
    for(const auto& node:hierarchy.prototypes) {
        NodeRecord n{node.key,node.geometry_key,node.expanded_triangles,
            {node.bounds.min.x,node.bounds.min.y,node.bounds.min.z,node.bounds.max.x,node.bounds.max.y,node.bounds.max.z},
            node.bounds.valid?1u:0u,node.subtree_depth,uint32_t(node.children.size()),uint32_t(node.levels.size())};
        write(f,n);
        for(const auto& child:node.children) {
            ChildRecord c{}; c.node=child.node; std::copy_n(child.transform.m,16,c.transform); write(f,c);
        }
        for(const auto& asset:node.levels) {
            const AssetRecord a{{asset.origin.x,asset.origin.y,asset.origin.z,asset.cell_size},uint32_t(asset.bricks.size()),uint32_t(asset.cells.size())};
            write(f,a);
            for(const auto& brick:asset.bricks) {
                const BrickRecord b{{brick.coord[0],brick.coord[1],brick.coord[2]},
                    {uint32_t(brick.mask),uint32_t(brick.mask>>32)},brick.first_cell};
                write(f,b);
            }
            std::vector<CellRecord> cells; cells.reserve(asset.cells.size());
            for(const auto& cell:asset.cells) cells.push_back(encode(cell));
            f.write(reinterpret_cast<const char*>(cells.data()),std::streamsize(cells.size()*sizeof(CellRecord)));
        }
    }
    if(!f) { error="failed writing sparse hierarchy diagnostic"; return false; }
    return true;
}
inline bool read(std::istream& f,sparse_voxel::Hierarchy& out,std::string& error) {
    error.clear();
    const auto fail=[&](const char* message) { error=message; return false; };
    uint64_t magic=0,stats[8]{}; uint32_t endian=0,nodes=0,roots=0;
    if(!read(f,magic) || magic!=0x3147414944485653ull || !read(f,endian) || endian!=0x01020304u ||
       !read(f,nodes) || !read(f,roots) || nodes>16384 || roots>16384 || !read(f,stats))
        return fail("invalid sparse hierarchy diagnostic header");
    sparse_voxel::Hierarchy result;
    result.roots.resize(roots); for(auto& root:result.roots)
        if(!read(f,root) || root>=nodes) return fail("invalid sparse hierarchy diagnostic root");
    uint64_t total_cells=0,total_links=0; uint32_t max_children=0,max_depth=0;
    result.prototypes.reserve(nodes);
    for(uint32_t index=0;index<nodes;++index) {
        NodeRecord n{};
        if(!read(f,n) || n.valid>1 || !n.depth || n.depth>64 || n.children>(1u<<20) ||
           n.children>(1u<<21)-total_links || !n.levels || n.levels>32)
            return fail("invalid sparse hierarchy diagnostic node");
        for(float v:n.bounds) if(!std::isfinite(v)) return fail("nonfinite sparse hierarchy diagnostic bounds");
        if(n.valid) for(int k=0;k<3;++k) if(n.bounds[k]>n.bounds[k+3]) return fail("unordered sparse hierarchy diagnostic bounds");
        sparse_voxel::Prototype node; node.key=n.key; node.geometry_key=n.geometry_key;
        node.expanded_triangles=n.expanded; node.subtree_depth=n.depth;
        node.bounds={{n.bounds[0],n.bounds[1],n.bounds[2]},{n.bounds[3],n.bounds[4],n.bounds[5]},n.valid!=0};
        node.children.reserve(n.children); uint32_t depth=1;
        for(uint32_t j=0;j<n.children;++j) {
            ChildRecord c{}; sparse_voxel::CompiledChild child;
            if(!read(f,c) || c.node>=index) return fail("invalid sparse hierarchy diagnostic child order");
            child.node=c.node; std::copy_n(c.transform,16,child.transform.m); double scale=0;
            if(!sparse_voxel::similarity_scale(child.transform,scale)) return fail("invalid sparse hierarchy diagnostic transform");
            depth=std::max(depth,result.prototypes[c.node].subtree_depth+1); node.children.push_back(child);
        }
        if(depth!=n.depth) return fail("incorrect sparse hierarchy diagnostic depth");
        total_links+=n.children; max_children=std::max(max_children,n.children); max_depth=std::max(max_depth,n.depth);
        float previous_spacing=0;
        for(uint32_t level=0;level<n.levels;++level) {
            AssetRecord a{};
            if(!read(f,a) || a.cells>(1u<<18) || a.bricks>a.cells || a.cells>(1u<<22)-total_cells ||
               !(a.grid[3]>previous_spacing)) return fail("invalid sparse hierarchy diagnostic asset counts or spacing");
            sparse_voxel::Asset asset; asset.origin={a.grid[0],a.grid[1],a.grid[2]}; asset.cell_size=a.grid[3];
            asset.bricks.resize(a.bricks);
            for(auto& brick:asset.bricks) {
                BrickRecord b{}; if(!read(f,b)) return fail("truncated sparse hierarchy diagnostic bricks");
                brick.coord={b.coord[0],b.coord[1],b.coord[2]}; brick.mask=uint64_t(b.mask[0])|(uint64_t(b.mask[1])<<32); brick.first_cell=b.first;
            }
            std::vector<CellRecord> cells(a.cells);
            if(!f.read(reinterpret_cast<char*>(cells.data()),std::streamsize(cells.size()*sizeof(CellRecord))))
                return fail("truncated sparse hierarchy diagnostic cells");
            asset.cells.reserve(cells.size());
            for(const auto& cell:cells) {
                if(cell.flags>3 || cell.reserved) return fail("invalid sparse hierarchy diagnostic cell flags");
                asset.cells.push_back(decode(cell));
            }
            if(!sparse_voxel::validate(asset,error)) return false;
            total_cells+=a.cells; previous_spacing=a.grid[3]; node.levels.push_back(std::move(asset));
        }
        result.prototypes.push_back(std::move(node));
    }
    if(total_cells!=stats[3] || total_links!=stats[5] || max_children!=stats[6] || max_depth!=stats[7] ||
       stats[0]>(1u<<24) || stats[1]>(1u<<20) || stats[4]>nodes || f.peek()!=std::char_traits<char>::eof())
        return fail("sparse hierarchy diagnostic census or trailing data mismatch");
    result.stats={stats[0],stats[1],stats[2],stats[3],stats[4],stats[5],uint32_t(stats[6]),uint32_t(stats[7])};
    out=std::move(result); return true;
}
}
inline bool write_sparse_hierarchy_fixture(const char* path,const sparse_voxel::Hierarchy& hierarchy,std::string& error) {
    std::ofstream f(path,std::ios::binary); if(!sparse_hierarchy_fixture::write(f,hierarchy,error)) return false;
    f.close(); if(!f) { error="failed closing sparse hierarchy diagnostic"; return false; } return true;
}
inline bool read_sparse_hierarchy_fixture(const char* path,sparse_voxel::Hierarchy& hierarchy,std::string& error) {
    std::ifstream f(path,std::ios::binary); return sparse_hierarchy_fixture::read(f,hierarchy,error);
}
