#include "geometry_hierarchy.h"
#include "asset_binary.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <chrono>
#include <cstdlib>
#include "matter/log.h"

namespace geometry {
namespace {
using namespace asset_store;
constexpr uint32_t node_kind=0x314F4547, manifest_kind=0x314D4547;
constexpr uint32_t meta_type=1,position_type=2,index_type=3,shading_type=4,children_type=5,roots_type=6,receiver_type=7;
constexpr uint32_t ref_stride=56;
void f32(std::vector<uint8_t>& b,float v) {uint32_t bits;std::memcpy(&bits,&v,4);push_u32(b,bits);}
float read_f32(const uint8_t* p) {const uint32_t bits=get_u32(p);float v;std::memcpy(&v,&bits,4);return v;}
void f64(std::vector<uint8_t>& b,double v) {uint64_t bits;std::memcpy(&bits,&v,8);push_u64(b,bits);}
double read_f64(const uint8_t* p) {const uint64_t bits=get_u64(p);double v;std::memcpy(&v,&bits,8);return v;}
void bounds_bytes(std::vector<uint8_t>& b,const Bounds& bounds) {for(float v:bounds.lo)f32(b,v);for(float v:bounds.hi)f32(b,v);}
Bounds read_bounds(const uint8_t* p) {Bounds b;for(int k=0;k<3;++k){b.lo[k]=read_f32(p+4*k);b.hi[k]=read_f32(p+12+4*k);}return b;}
bool valid_bounds(const Bounds& b) {for(int k=0;k<3;++k)if(!std::isfinite(b.lo[k])||!std::isfinite(b.hi[k])||b.lo[k]>b.hi[k])return false;return true;}
void ref_bytes(std::vector<uint8_t>& b,const NodeRef& r) {
    push_u64(b,r.page.lo);push_u64(b,r.page.hi);bounds_bytes(b,r.bounds);f64(b,r.error);
    push_u32(b,r.source_triangles);push_u32(b,r.triangles);
}
bool read_ref(const uint8_t* p,NodeRef& r) {
    r.page={get_u64(p),get_u64(p+8)};r.bounds=read_bounds(p+16);r.error=read_f64(p+40);
    r.source_triangles=get_u32(p+48);r.triangles=get_u32(p+52);
    return r.page.valid()&&valid_bounds(r.bounds)&&std::isfinite(r.error)&&r.error>=0&&r.source_triangles&&r.triangles&&r.triangles<=r.source_triangles;
}
bool fail(std::string& e,const char* s){e=s;return false;}
bool section(const PageSectionView* s,uint32_t stride){return s&&s->schema==1&&s->stride==stride;}
void vec3(std::vector<uint8_t>& b,float3 v){f32(b,v.x);f32(b,v.y);f32(b,v.z);}
float3 read_vec3(const uint8_t* p){return make_float3(read_f32(p),read_f32(p+4),read_f32(p+8));}
}
bool encode_node(const Node& n,const std::vector<NodeRef>& children,const asset_store::PageLimits& limits,
                 std::vector<uint8_t>& out,std::string& error) {
    using namespace asset_store;
    if(n.mesh.indices.empty()||n.mesh.indices.size()%3||n.mesh.triex.size()!=n.mesh.indices.size()/3||
       n.mesh.positions.size()>UINT32_MAX||n.mesh.indices.size()>UINT32_MAX||
       !valid_bounds(n.bounds)||!std::isfinite(n.error)||n.error<0||(!children.empty()&&children.size()!=2)||
       n.source_triangles<n.mesh.indices.size()/3) return fail(error,"invalid geometry node");
    std::vector<PageSection> sections;
    PageSection meta{meta_type,1,40,{}};bounds_bytes(meta.bytes,n.bounds);f64(meta.bytes,n.error);
    push_u32(meta.bytes,n.source_triangles);push_u32(meta.bytes,static_cast<uint32_t>(n.mesh.indices.size()/3));sections.push_back(std::move(meta));
    PageSection vertices{position_type,1,12,{}};for(auto p:n.mesh.positions)vec3(vertices.bytes,p);sections.push_back(std::move(vertices));
    PageSection indices{index_type,1,4,{}};for(auto i:n.mesh.indices)push_u32(indices.bytes,i);sections.push_back(std::move(indices));
    PageSection shading{shading_type,1,92,{}};
    for(const auto& ex:n.mesh.triex) {
        const float2 uv[]={ex.uv0,ex.uv1,ex.uv2};const float3 normals[]={ex.N0,ex.N1,ex.N2};const float ao[]={ex.ao0,ex.ao1,ex.ao2};
        for(int k=0;k<3;++k){f32(shading.bytes,uv[k].x);f32(shading.bytes,uv[k].y);vec3(shading.bytes,normals[k]);f32(shading.bytes,ao[k]);}
        push_u32(shading.bytes,static_cast<uint32_t>(ex.materialId));
        f32(shading.bytes,ex.tint.x);f32(shading.bytes,ex.tint.y);f32(shading.bytes,ex.tint.z);f32(shading.bytes,ex.tint.w);
    }
    sections.push_back(std::move(shading));
    if(!n.receivers.empty()) {
        if(n.receivers.size()!=n.mesh.indices.size())return fail(error,"receiver mapping count differs from geometry corners");
        PageSection receiver{receiver_type,1,24,{}};
        for(const auto& corner:n.receivers){vec3(receiver.bytes,corner.position);vec3(receiver.bytes,corner.normal);}
        sections.push_back(std::move(receiver));
    }
    PageSection descendants{children_type,1,ref_stride,{}};std::set<BlobHash> dependencies;
    for(const auto& child:children){ref_bytes(descendants.bytes,child);dependencies.insert(child.page);}
    if(!children.empty())sections.push_back(std::move(descendants));
    std::vector<uint8_t> encoded;
    if(!encode_page(node_kind,sections,{dependencies.begin(),dependencies.end()},limits,encoded,error))return false;
    auto page=std::make_shared<CachedPage>();
    page->hash=hash_bytes(encoded.data(),encoded.size());page->bytes=encoded.data();page->size=encoded.size();
    if(!decode_page(encoded.data(),encoded.size(),limits,page->view,error))return false;
    NodeView checked;if(!decode_node(page,checked,error))return false;
    out=std::move(encoded);return true;
}
bool decode_node(asset_store::PageHandle page,NodeView& out,std::string& error) {
    using namespace asset_store;
    if(!page||page->view.kind!=node_kind) return fail(error,"not a geometry page");
    const auto& v=page->view;
    const auto* m=v.find(meta_type);const auto* p=v.find(position_type);const auto* i=v.find(index_type);const auto* s=v.find(shading_type);const auto* c=v.find(children_type);
    const auto* r=v.find(receiver_type);
    if(r && (!section(r,24) || !i || r->count!=i->count))return fail(error,"invalid receiver page section");
    if(!section(m,40)||m->count!=1||!section(p,12)||!section(i,4)||!section(s,92)||i->count%3||
       s->count!=i->count/3||v.sections.size()!=(c?5u:4u)+(r?1u:0u)|| (c&&(!section(c,ref_stride)||c->count!=2))) return fail(error,"invalid geometry page directory");
    NodeView node;node.page=page;node.self.page=page->hash;node.self.bounds=read_bounds(m->data);node.self.error=read_f64(m->data+24);
    node.self.source_triangles=get_u32(m->data+32);node.self.triangles=get_u32(m->data+36);
    if(!valid_bounds(node.self.bounds)||!std::isfinite(node.self.error)||node.self.error<0||
       node.self.triangles!=s->count||node.self.source_triangles<node.self.triangles) return fail(error,"invalid geometry metadata");
    if(r)for(uint32_t corner=0;corner<r->count;++corner) {
        for(int k=0;k<6;++k)if(!std::isfinite(read_f32(r->data+size_t(corner)*24+k*4)))return fail(error,"nonfinite receiver mapping");
        const auto n=read_vec3(r->data+size_t(corner)*24+12);
        if(double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z<1e-12)return fail(error,"zero receiver normal");
    }
    for(uint32_t vertex=0;vertex<p->count;++vertex)for(int k=0;k<3;++k) {
        const float x=read_f32(p->data+size_t(vertex)*12+k*4);
        if(!std::isfinite(x)||x<node.self.bounds.lo[k]||x>node.self.bounds.hi[k])return fail(error,"geometry vertex outside bounds");
    }
    for(uint32_t index=0;index<i->count;++index)if(get_u32(i->data+size_t(index)*4)>=p->count)return fail(error,"geometry page index outside vertices");
    for(uint32_t t=0;t<s->count;++t) {
        const auto a=read_vec3(p->data+size_t(get_u32(i->data+size_t(t)*12))*12);
        const auto b=read_vec3(p->data+size_t(get_u32(i->data+size_t(t)*12+4))*12);
        const auto c0=read_vec3(p->data+size_t(get_u32(i->data+size_t(t)*12+8))*12);
        const double ab[]={double(b.x)-a.x,double(b.y)-a.y,double(b.z)-a.z};
        const double ac[]={double(c0.x)-a.x,double(c0.y)-a.y,double(c0.z)-a.z};
        const double cross[]={ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
        if(cross[0]==0&&cross[1]==0&&cross[2]==0)return fail(error,"degenerate geometry page triangle");
    }
    for(uint32_t t=0;t<s->count;++t)for(uint32_t byte=0;byte<92;byte+=4)
        if(byte!=72&&!std::isfinite(read_f32(s->data+size_t(t)*92+byte)))return fail(error,"nonfinite geometry shading");
    std::set<BlobHash> dependencies;uint64_t covered=0;
    if(c)for(uint32_t child=0;child<c->count;++child) {
        NodeRef ref;if(!read_ref(c->data+size_t(child)*ref_stride,ref)||ref.error>node.self.error||ref.page==page->hash)return fail(error,"invalid geometry replacement");
        for(int k=0;k<3;++k)if(ref.bounds.lo[k]<node.self.bounds.lo[k]||ref.bounds.hi[k]>node.self.bounds.hi[k])return fail(error,"child outside conservative parent bounds");
        covered+=ref.source_triangles;dependencies.insert(ref.page);node.children.push_back(ref);
    }
    if((!c&&node.self.source_triangles!=node.self.triangles)||(c&&covered!=node.self.source_triangles)||std::vector<BlobHash>(dependencies.begin(),dependencies.end())!=v.dependencies)
        return fail(error,"geometry coverage/dependency mismatch");
    out=std::move(node);error.clear();return true;
}
bool decode_receivers(const NodeView& node,std::vector<ReceiverCorner>& out,std::string& error) {
    NodeView validated;if(!decode_node(node.page,validated,error))return false;
    std::vector<ReceiverCorner> result;
    if(const auto* lane=node.page->view.find(receiver_type)) {
        if(!section(lane,24) || uint64_t(lane->count)!=uint64_t(validated.self.triangles)*3u)return fail(error,"invalid receiver mapping");
        for(uint32_t i=0;i<lane->count;++i)result.push_back({read_vec3(lane->data+size_t(i)*24),read_vec3(lane->data+size_t(i)*24+12)});
    }
    out=std::move(result);error.clear();return true;
}
bool decode_mesh(const NodeView& node,MeshIndexed& out,std::string& error) {
    NodeView validated;if(!decode_node(node.page,validated,error))return false;
    const auto& view=node.page->view;MeshIndexed mesh;
    const auto& positions=*view.find(position_type);const auto& indices=*view.find(index_type);const auto& shading=*view.find(shading_type);
    mesh.positions.reserve(positions.count);for(uint32_t i=0;i<positions.count;++i)mesh.positions.push_back(read_vec3(positions.data+size_t(i)*12));
    mesh.indices.reserve(indices.count);for(uint32_t i=0;i<indices.count;++i)mesh.indices.push_back(asset_store::get_u32(indices.data+size_t(i)*4));
    mesh.triex.resize(shading.count);
    for(uint32_t i=0;i<shading.count;++i) {
        const auto* row=shading.data+size_t(i)*92;auto& ex=mesh.triex[i];
        float2* uv[]={&ex.uv0,&ex.uv1,&ex.uv2};float3* n[]={&ex.N0,&ex.N1,&ex.N2};float* ao[]={&ex.ao0,&ex.ao1,&ex.ao2};
        for(int k=0;k<3;++k){*uv[k]={read_f32(row+k*24),read_f32(row+k*24+4)};*n[k]=read_vec3(row+k*24+8);*ao[k]=read_f32(row+k*24+20);}
        ex.materialId=static_cast<int32_t>(asset_store::get_u32(row+72));ex.tint=make_float4(read_f32(row+76),read_f32(row+80),read_f32(row+84),read_f32(row+88));
    }
    out=std::move(mesh);error.clear();return true;
}
bool write_hierarchy(const Hierarchy& hierarchy,asset_store::BlobStore& store,asset_store::RefTable& refs,
                     const std::string& key,const asset_store::PageLimits& limits,std::vector<NodeRef>& roots,std::string& error,
                     const std::vector<asset_store::PageSection>& metadata, bool commit,
                     std::vector<NodeRef>* all_refs, std::vector<uint8_t>* manifest_bytes) {
    using namespace asset_store;
    if(hierarchy.nodes.empty()||hierarchy.roots.empty())return fail(error,"empty hierarchy");
    std::vector<uint32_t> parents(hierarchy.nodes.size(),0);
    for(size_t id=0;id<hierarchy.nodes.size();++id)for(auto child:hierarchy.nodes[id].children)
        if(child>=id || ++parents[child]!=1)return fail(error,"invalid tree replacement ownership");
    std::set<uint32_t> root_ids;
    for(auto root:hierarchy.roots)
        if(root>=parents.size()||parents[root]||!root_ids.insert(root).second)return fail(error,"invalid tree roots");
    for(uint32_t id=0;id<parents.size();++id)
        if(!parents[id]&&!root_ids.count(id))return fail(error,"unreachable geometry node");
    std::vector<NodeRef> encoded;encoded.reserve(hierarchy.nodes.size());
    std::vector<std::vector<uint8_t>> batch;size_t batch_bytes=0;
    const size_t max_batch=8u*1024*1024;
    using Clock=std::chrono::steady_clock;
    const auto elapsed=[](auto t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();};
    double encode_ms=0,put_ms=0;uint64_t encoded_bytes=0;size_t batches=0;
    const auto flush=[&]() {
        const auto started=Clock::now();
        std::vector<BlobInput> inputs;for(const auto& b:batch)inputs.push_back({b.data(),b.size()});
        std::vector<BlobHash> hashes;
        if(store.put_batch(inputs,max_batch,hashes)!=Status::Ok){error=store.last_error();return false;}
        put_ms+=elapsed(started);++batches;
        batch.clear();batch_bytes=0;return true;
    };
    for(size_t id=0;id<hierarchy.nodes.size();++id) {
        const auto& n=hierarchy.nodes[id];std::vector<NodeRef> children;
        for(auto child:n.children){if(child>=id)return fail(error,"hierarchy is not child-first acyclic");children.push_back(encoded[child]);}
        const auto started=Clock::now();
        std::vector<uint8_t> bytes;if(!encode_node(n,children,limits,bytes,error))return false;
        encode_ms+=elapsed(started);encoded_bytes+=bytes.size();
        const size_t record_bytes=(bytes.size()+32+7)&~size_t(7);
        if(record_bytes>max_batch)return fail(error,"geometry page exceeds write admission limit");
        if(batch_bytes+record_bytes>max_batch&&!flush())return false;
        encoded.push_back({hash_bytes(bytes.data(),bytes.size()),n.bounds,n.error,n.source_triangles,static_cast<uint32_t>(n.mesh.indices.size()/3)});
        batch_bytes+=record_bytes;batch.push_back(std::move(bytes));
    }
    if(!batch.empty()&&!flush())return false;
    std::vector<NodeRef> root_refs;std::set<BlobHash> dependencies;PageSection root_section{roots_type,1,ref_stride,{}};
    for(auto root:hierarchy.roots){if(root>=encoded.size())return fail(error,"invalid hierarchy root");const auto& r=encoded[root];root_refs.push_back(r);dependencies.insert(r.page);ref_bytes(root_section.bytes,r);}
    std::vector<uint8_t> manifest;
    std::vector<PageSection> sections{std::move(root_section)};
    for (const auto& section : metadata) {
        if (section.type < 0x80000000u) return fail(error,"geometry manifest metadata uses a reserved section type");
        sections.push_back(section);
    }
    if(!encode_page(manifest_kind,sections,{dependencies.begin(),dependencies.end()},limits,manifest,error))return false;
    BlobHash manifest_hash;
    const auto publish_start=Clock::now();
    if(!publish_page_manifest(store,refs,key,manifest,limits,manifest_hash,error,commit))return false;
    if(std::getenv("MATTER_GEOMETRY_PAGES_PROFILE"))MATTER_LOGI("geometry","page_write_profile key=%s nodes=%zu bytes=%llu batches=%zu encode_ms=%.3f put_ms=%.3f publish_ms=%.3f",key.c_str(),hierarchy.nodes.size(),(unsigned long long)encoded_bytes,batches,encode_ms,put_ms,elapsed(publish_start));
    if(all_refs)*all_refs=std::move(encoded);
    if(manifest_bytes)*manifest_bytes=std::move(manifest);
    roots=std::move(root_refs);return true;
}
bool decode_roots(asset_store::PageHandle manifest,std::vector<NodeRef>& roots,std::string& error) {
    if(!manifest||manifest->view.kind!=manifest_kind)return fail(error,"invalid geometry manifest");
    for (const auto& section : manifest->view.sections)
        if (section.type != roots_type && section.type < 0x80000000u) return fail(error,"unknown geometry manifest section");
    const auto* section_view=manifest->view.find(roots_type);if(!section(section_view,ref_stride))return fail(error,"invalid roots directory");
    std::vector<NodeRef> result;std::set<asset_store::BlobHash> dependencies;
    for(uint32_t i=0;i<section_view->count;++i){NodeRef r;if(!read_ref(section_view->data+size_t(i)*ref_stride,r))return fail(error,"invalid geometry root");result.push_back(r);dependencies.insert(r.page);}
    if(std::vector<asset_store::BlobHash>(dependencies.begin(),dependencies.end())!=manifest->view.dependencies)return fail(error,"root dependencies mismatch");
    roots=std::move(result);error.clear();return true;
}
} // namespace geometry
