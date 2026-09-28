#pragma once
// Derived, same-ABI sector records in AssetStore binary packs. Payload layout
// has its own policy key and explicit ABI signature; no pointers are persisted.
// Rebuild incompatible records. Reads use preallocated, independently evictable payload banks.
#include "part_store.h"
#include "asset_binary.h"
#include <filesystem>
#include <array>
#include <functional>
#include <chrono>
#include <cstdlib>
#include <type_traits>
#include <stdexcept>

namespace viewer::prepared_sector {
constexpr uint32_t kind = 0x3153504d;
constexpr uint32_t max_bytes = 64u << 20;
struct Archive {
    bool reading = false;
    std::vector<uint8_t> bytes;
    const uint8_t* data = nullptr;
    size_t size = 0, at = 0;
    template<class T> void pod(T& value) {
        static_assert(std::is_trivially_copyable<T>::value, "cache pointers forbidden");
        if (reading) {
            if (at > size || sizeof(T) > size-at) throw std::runtime_error("truncated sector field");
            std::memcpy(&value,data+at,sizeof(T)); at+=sizeof(T);
        } else {
            if (bytes.size()+sizeof(T)>max_bytes) throw std::runtime_error("sector exceeds byte limit");
            const auto* p=reinterpret_cast<const uint8_t*>(&value);bytes.insert(bytes.end(),p,p+sizeof(T));
        }
    }
    template<class T> void vec(std::vector<T>& values) {
        static_assert(std::is_trivially_copyable<T>::value, "cache vector must be scalar/POD");
        uint32_t n=static_cast<uint32_t>(values.size());pod(n);
        if(n>max_bytes/sizeof(T))throw std::runtime_error("sector vector exceeds byte limit");
        if(reading){if(at>size || uint64_t(n)*sizeof(T)>size-at)throw std::runtime_error("truncated sector vector");values.resize(n);if(n)std::memcpy(values.data(),data+at,n*sizeof(T));at+=n*sizeof(T);}
        else {if(bytes.size()+uint64_t(n)*sizeof(T)>max_bytes)throw std::runtime_error("sector exceeds byte limit");if(n){const auto* p=reinterpret_cast<const uint8_t*>(values.data());bytes.insert(bytes.end(),p,p+n*sizeof(T));}}
    }
    template<class T,class F> void list(std::vector<T>& values,F f){uint32_t n=static_cast<uint32_t>(values.size());pod(n);if(n>1048576 || (reading && n>size-at))throw std::runtime_error("sector list count");if(reading)values.resize(n);for(auto& x:values)f(x);}
    void string(std::string& s){std::vector<char> v(s.begin(),s.end());vec(v);if(reading)s.assign(v.begin(),v.end());}
};
inline void mesh(Archive& a,RasterMeshData& m){
    a.pod(m.vertex_count);a.vec(m.vertices);a.vec(m.normals);a.vec(m.colors);a.vec(m.texcoords);
    a.vec(m.surface_uvs);a.vec(m.material_ids);a.vec(m.baked_ao);a.vec(m.indices);a.vec(m.warp_uvs);a.vec(m.warp_frames);
    if(m.indices.size()%3)throw std::runtime_error("sector triangle count");
    if(m.vertex_count<0 || m.vertices.size()!=size_t(m.vertex_count)*3 || m.normals.size()!=m.vertices.size() ||
       m.colors.size()!=size_t(m.vertex_count)*4 || m.texcoords.size()!=size_t(m.vertex_count)*2 ||
       (!m.warp_uvs.empty() && m.warp_uvs.size()!=size_t(m.vertex_count)*2) ||
       (!m.warp_frames.empty() && m.warp_frames.size()!=size_t(m.vertex_count)*2))throw std::runtime_error("sector mesh channels");
    for(auto i:m.indices)if(i>=uint32_t(m.vertex_count))throw std::runtime_error("sector mesh index");
}
inline void boundary(Archive& a,std::shared_ptr<const seam::SectorBoundary>& value){
    uint32_t present=value?1:0;a.pod(present);if(present>1)throw std::runtime_error("sector boundary flag");if(!present)return;
    auto b=a.reading?std::make_shared<seam::SectorBoundary>():std::make_shared<seam::SectorBoundary>(*value);
    a.pod(b->rung);a.pod(b->cells);a.pod(b->tx);a.pod(b->ty);a.pod(b->tz);uint32_t tiled=b->y_tiled;a.pod(tiled);b->y_tiled=tiled!=0;
    for(auto& f:b->faces){a.pod(f.face);a.pod(f.plane);a.pod(f.cell_layer);
        a.list(f.verts,[&](auto& v){a.pod(v.a);a.pod(v.b);a.pod(v.px);a.pod(v.py);a.pod(v.pz);a.pod(v.nx);a.pod(v.ny);a.pod(v.nz);a.pod(v.material);a.pod(v.corner_signs);});
        a.list(f.band.buckets,[&](auto& v){a.pod(v.material);a.vec(v.positions);a.vec(v.normals);if(v.positions.size()%9 || v.positions.size()!=v.normals.size())throw std::runtime_error("sector overlap band");});}
    if(a.reading)value=std::move(b);
}
inline void transfer(Archive& a,PartStore::StagedPart& s,std::string& geometry_key,asset_store::BlobHash& manifest){
    uint64_t abi=(uint64_t(sizeof(Tri))<<48)|(uint64_t(sizeof(TriEx))<<32)|(uint64_t(sizeof(BVHNode))<<16)|sizeof(chart_atlas::ChartEntry);
    auto expected=abi;a.pod(abi);if(abi!=expected)throw std::runtime_error("sector ABI mismatch");
    a.pod(s.part_hash);auto& p=s.lp;
    if(a.reading)s.staging=std::make_unique<BLASManager>();
    uint32_t entries=a.reading?0:static_cast<uint32_t>(s.staging->get_entries().size());a.pod(entries);
    if(entries>64)throw std::runtime_error("sector BLAS count");
    for(uint32_t i=0;i<entries;++i){
        std::vector<Tri> tris;std::vector<TriEx> extra;std::vector<BVHNode> nodes;std::vector<uint32_t> order;uint64_t hash=0;uint32_t refs=0;
        if(!a.reading){const auto& e=s.staging->get_entries()[i];if(!e || !e->bvh || e->handle!=i+1)throw std::runtime_error("sector BLAS hole");
            tris=e->triangles;extra=e->tri_extra;nodes.assign(e->bvh->bvhNode,e->bvh->bvhNode+e->bvh->nodesUsed);order.assign(e->bvh->triIdx,e->bvh->triIdx+tris.size());hash=e->hash;refs=e->ref_count;}
        // Named triangle fields exclude SIMD padding and TriEx tail padding.
        a.list(tris,[&](auto& t){a.pod(t.vertex0);a.pod(t.vertex1);a.pod(t.vertex2);a.pod(t.centroid);});
        a.list(extra,[&](auto& t){a.pod(t.N0);a.pod(t.N1);a.pod(t.N2);a.pod(t.tint);a.pod(t.materialId);a.pod(t.uv0);a.pod(t.uv1);a.pod(t.uv2);a.pod(t.ao0);a.pod(t.ao1);a.pod(t.ao2);});
        a.vec(nodes);a.vec(order);a.pod(hash);a.pod(refs);
        if(tris.empty() || extra.size()!=tris.size() || order.size()!=tris.size() || nodes.empty() || !refs)throw std::runtime_error("sector BLAS arrays");
        for(size_t n=0;n<nodes.size();++n){if(n==1)continue;const auto& node=nodes[n];
            if(node.triCount){if(uint64_t(node.leftFirst)+node.triCount>order.size())throw std::runtime_error("sector BVH leaf range");}
            else if(node.leftFirst<=n || uint64_t(node.leftFirst)+1>=nodes.size())throw std::runtime_error("sector BVH child range");}
        for(auto index:order)if(index>=tris.size())throw std::runtime_error("sector BVH index");
        if(a.reading && s.staging->register_prebuilt(tris.data(),extra.data(),int(tris.size()),nodes.data(),uint32_t(nodes.size()),order.data(),hash,refs)!=i+1)throw std::runtime_error("sector BLAS registration");
    }
    a.pod(p.bound_radius);a.vec(p.thresholds);a.vec(p.lod_blas);a.vec(p.owned_blas);
    a.pod(p.fine_cluster_count);uint32_t rt=p.render_policy.ray_traced;a.pod(rt);p.render_policy.ray_traced=rt!=0;a.pod(p.render_policy.vt_texels_per_meter);
    a.list(p.lod_mesh_data,[&](auto& m){mesh(a,m);});
    a.list(p.lod_charts,[&](auto& c){a.pod(c.atlas_w);a.pod(c.atlas_h);a.vec(c.charts);a.vec(c.tri_order);});
    a.list(p.clusters,[&](auto& c){a.pod(c.aabb_min);a.pod(c.aabb_max);a.pod(c.radius);a.vec(c.thresholds);a.vec(c.lod_blas);a.vec(c.lod_mesh);});
    boundary(a,p.boundary);
    a.pod(p.surface_cache.tape_hash);a.pod(p.surface_cache.material_count);a.pod(p.surface_cache.lane_count);
    a.list(p.surface_cache.weights,[&](auto& v){a.vec(v);});a.list(p.surface_cache.lanes,[&](auto& v){a.vec(v);});
    a.string(geometry_key);a.pod(manifest.lo);a.pod(manifest.hi);
    for(auto h:p.lod_blas)if(!s.staging->get_entry(h))throw std::runtime_error("sector LOD BLAS");
    for(auto h:p.owned_blas)if(!s.staging->get_entry(h))throw std::runtime_error("sector owned BLAS");
    for(const auto& c:p.clusters){for(auto h:c.lod_blas)if(!s.staging->get_entry(h))throw std::runtime_error("sector cluster BLAS");for(auto m:c.lod_mesh)if(m<0 || size_t(m)>=p.lod_mesh_data.size())throw std::runtime_error("sector cluster mesh");}
    if(p.lod_blas.size()!=p.thresholds.size() || p.lod_blas.size()!=p.lod_charts.size())throw std::runtime_error("sector LOD arrays");
    if(a.reading && a.at!=a.size)throw std::runtime_error("trailing sector data");s.ok=true;
}
struct ReadTiming { double wait_ms=0, io_ms=0; };
class Cache {
    asset_store::PageCacheConfig cfg_;
    struct Reader { std::shared_ptr<asset_store::PageBank> bank; std::unique_ptr<asset_store::PageCache> cache; std::mutex mutex; };
    std::array<Reader,8> readers_;
    uint32_t reader_count_=1;
    std::mutex writer_mutex_;
public:
    explicit Cache(std::string dir, uint32_t readers=0, uint64_t reader_bytes=128ull<<20) {
        if (!readers) {
            if (const char* value=std::getenv("MATTER_PREPARED_SECTOR_READERS")) {
                char* end=nullptr; const auto n=std::strtoul(value,&end,10);
                if(end!=value && !*end && n>=1 && n<=8) readers=uint32_t(n);
            }
        }
        reader_count_=readers>=1 && readers<=8 ? readers : 1;
        cfg_.store.dir=std::move(dir); cfg_.resident_bytes=reader_bytes;
        // Independent eviction domains need independent address ranges. Sharing
        // an unpartitioned bank lets other readers strand free space in small
        // holes that this reader cannot coalesce by evicting its own entries.
        // Reserve every bank up front; reads never allocate backing storage.
        for(uint32_t i=0;i<reader_count_;++i) {
            readers_[i].bank=asset_store::PageBank::create(cfg_.resident_bytes,256);
            if(!readers_[i].bank)throw std::runtime_error("prepared sector bank allocation failed");
        }
        cfg_.limits.max_bytes=max_bytes; cfg_.max_read_bytes=max_bytes;
        if (const char* value=std::getenv("MATTER_PREPARED_SECTOR_READ_AHEAD_MB")) {
            char* end=nullptr; const auto mb=std::strtoul(value,&end,10);
            if (end!=value && !*end && mb<=16) cfg_.read_ahead_bytes=uint32_t(mb)<<20;
        }
        if (cfg_.read_ahead_bytes) cfg_.store.batch_max_bytes=cfg_.read_ahead_bytes;
    }
    asset_store::PageHandle read(const std::string& key,std::string& error,ReadTiming* timing=nullptr){
        const auto start=std::chrono::steady_clock::now();
        auto& reader=readers_[std::hash<std::string>{}(key)%reader_count_];
        std::lock_guard<std::mutex> lock(reader.mutex);
        const auto acquired=std::chrono::steady_clock::now();
        if(timing)timing->wait_ms=std::chrono::duration<double,std::milli>(acquired-start).count();
        if(!reader.cache){if(!std::filesystem::exists(cfg_.store.dir)){error="missing";return {};}
            auto config=cfg_;config.bank=reader.bank;
            reader.cache=asset_store::PageCache::open(config,error);}
        if(!reader.cache)return {};
        // PageCache reuses unchanged references and observes atomic updates.
        auto result=reader.cache->read_manifest(key);
        if(timing)timing->io_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-acquired).count();
        if(!result.page){
            const char* status="unknown";
            switch(result.status){
            case asset_store::PageStatus::Ok: status="empty success";break;
            case asset_store::PageStatus::Missing: status="missing";break;
            case asset_store::PageStatus::Corrupt: status="corrupt";break;
            case asset_store::PageStatus::IoError: status="I/O error";break;
            case asset_store::PageStatus::BudgetExceeded: status="budget exceeded";break;
            case asset_store::PageStatus::Cancelled: status="cancelled";break;
            }
            const auto bank=reader.bank->stats();
            error=std::string("prepared sector payload unavailable: ")+status+
                " bank_occupied="+std::to_string(bank.occupied)+
                " bank_largest_free="+std::to_string(bank.largest_free)+
                " bank_active="+std::to_string(bank.active);
            return {};
        }
        error.clear();return result.page;
    }
    bool write(const std::string& key,std::vector<uint8_t> payload,std::string& error){
        std::vector<uint8_t> bytes;asset_store::PageSection section;section.type=1;section.bytes=std::move(payload);
        if(!asset_store::encode_page(kind,{section},{},cfg_.limits,bytes,error))return false;
        std::lock_guard<std::mutex> lock(writer_mutex_);
        auto store=asset_store::BlobStore::open(cfg_.store,&error);if(!store)return false;
        asset_store::BlobHash hash;
        if(store->put(bytes.data(),bytes.size(),&hash)!=asset_store::Status::Ok || !store->flush_index())return false;
        auto refs=asset_store::RefTable::open(*store,{},&error);
        const bool committed=refs && refs->put(key,hash,kind,bytes.size()) && refs->flush();
        // read_manifest observes atomically published references and refreshes
        // its index before reading. Do not touch another reader's mutable state.
        return committed;
    }
};
}
