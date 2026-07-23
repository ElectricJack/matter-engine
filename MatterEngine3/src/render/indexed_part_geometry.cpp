#include "indexed_part_geometry.h"

#include <cstring>
#include <unordered_map>

namespace {
struct VertexKey { float px,py,pz,nx,ny,nz,tc0,tc1,su,sv,ao; unsigned char rgba[4]; uint32_t material_id; };
static_assert(sizeof(VertexKey) == 52, "deterministic exact-bit weld key");
struct VertexKeyHash { size_t operator()(const VertexKey& key) const { const auto* b=reinterpret_cast<const unsigned char*>(&key); size_t h=1469598103934665603ull; for(size_t i=0;i<sizeof(key);++i){h^=b[i];h*=1099511628211ull;} return h; } };
struct VertexKeyEq { bool operator()(const VertexKey& a,const VertexKey& b) const { return std::memcmp(&a,&b,sizeof(a))==0; } };
unsigned char to_u8(float v) { const float c=v<0?0:(v>1?1:v); return static_cast<unsigned char>(c*255.0f+0.5f); }
void append(viewer::IndexedPartGeometry& out,const VertexKey& key) { out.vertices.insert(out.vertices.end(),{key.px,key.py,key.pz});out.normals.insert(out.normals.end(),{key.nx,key.ny,key.nz});out.colors.insert(out.colors.end(),{key.rgba[0],key.rgba[1],key.rgba[2],key.rgba[3]});out.texcoords.insert(out.texcoords.end(),{key.tc0,key.tc1});out.surface_uvs.insert(out.surface_uvs.end(),{key.su,key.sv});out.material_ids.push_back(key.material_id);out.baked_ao.push_back(key.ao); }
void hash_bytes(uint64_t& h,const void* value,size_t count) { const auto* b=static_cast<const unsigned char*>(value); for(size_t i=0;i<count;++i){h^=b[i];h*=1099511628211ull;} }
template<class T> void hash_scalar(uint64_t& h,const T& v) { hash_bytes(h,&v,sizeof(v)); }
template<class T> void hash_vector(uint64_t& h,const std::vector<T>& v) { const uint64_t size=v.size();hash_scalar(h,size);if(!v.empty())hash_bytes(h,v.data(),v.size()*sizeof(T)); }
}

namespace viewer {
IndexedPartGeometry build_indexed_part_geometry(const Tri* tris,const TriEx* triex,int tri_count,float default_mat_id) {
    IndexedPartGeometry out; if(!tris||tri_count<=0)return out; const int estimate=tri_count*2;
    out.vertices.reserve(estimate*3);out.normals.reserve(estimate*3);out.colors.reserve(estimate*4);out.texcoords.reserve(estimate*2);out.surface_uvs.reserve(estimate*2);out.material_ids.reserve(estimate);out.baked_ao.reserve(estimate);out.indices.reserve(tri_count*3);
    std::unordered_map<VertexKey,uint32_t,VertexKeyHash,VertexKeyEq> weld; weld.reserve(tri_count*2);
    for(int i=0;i<tri_count;++i) { const Tri& t=tris[i];const float3 positions[3]={t.vertex0,t.vertex1,t.vertex2};float3 normals[3];if(triex){normals[0]=triex[i].N0;normals[1]=triex[i].N1;normals[2]=triex[i].N2;}else{const float3 normal=normalize(cross(t.vertex1-t.vertex0,t.vertex2-t.vertex0));normals[0]=normals[1]=normals[2]=normal;}const float ao[3]={triex?triex[i].ao0:1.0f,triex?triex[i].ao1:1.0f,triex?triex[i].ao2:1.0f};const float material=triex?static_cast<float>(triex[i].materialId):default_mat_id;const float2 uv[3]={triex?triex[i].uv0:make_float2(0.0f),triex?triex[i].uv1:make_float2(0.0f),triex?triex[i].uv2:make_float2(0.0f)};const uint32_t material_id=triex?static_cast<uint32_t>(triex[i].materialId):(default_mat_id>=0?static_cast<uint32_t>(default_mat_id+.5f):UINT32_MAX);unsigned char rgba[4]={255,255,255,0};if(triex){rgba[0]=to_u8(triex[i].tint.x);rgba[1]=to_u8(triex[i].tint.y);rgba[2]=to_u8(triex[i].tint.z);rgba[3]=to_u8(triex[i].tint.w);}for(int c=0;c<3;++c){VertexKey key{};key.px=positions[c].x;key.py=positions[c].y;key.pz=positions[c].z;key.nx=normals[c].x;key.ny=normals[c].y;key.nz=normals[c].z;key.tc0=material;key.tc1=ao[c];key.su=uv[c].x;key.sv=uv[c].y;key.ao=ao[c];std::memcpy(key.rgba,rgba,4);key.material_id=material_id;auto [entry,inserted]=weld.try_emplace(key,static_cast<uint32_t>(out.material_ids.size()));if(inserted)append(out,key);out.indices.push_back(entry->second);}}
    out.vertex_count=static_cast<int>(out.material_ids.size()); return out;
}
uint64_t indexed_part_geometry_signature(const IndexedPartGeometry& geometry,uint32_t lod_ordinal) { uint64_t h=1469598103934665603ull;const uint32_t tag=0x49444731u;hash_scalar(h,tag);hash_scalar(h,lod_ordinal);hash_scalar(h,geometry.vertex_count);hash_vector(h,geometry.vertices);hash_vector(h,geometry.normals);hash_vector(h,geometry.colors);hash_vector(h,geometry.texcoords);hash_vector(h,geometry.surface_uvs);hash_vector(h,geometry.material_ids);hash_vector(h,geometry.baked_ao);hash_vector(h,geometry.indices);return h?h:1ull; }
} // namespace viewer
