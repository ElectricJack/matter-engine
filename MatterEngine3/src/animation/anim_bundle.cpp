#include "animation/anim_bundle.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace matter::animation {
namespace {
uint64_t fnv(const uint8_t* p, size_t n) { uint64_t h=1469598103934665603ull; for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;} return h; }
void u32(std::vector<uint8_t>& b,uint32_t v){for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));}
void u64(std::vector<uint8_t>& b,uint64_t v){for(int i=0;i<8;++i)b.push_back(uint8_t(v>>(8*i)));}
bool g32(const std::vector<uint8_t>&b,size_t&p,uint32_t&v){if(p>b.size()||b.size()-p<4)return false;v=0;for(int i=0;i<4;++i)v|=uint32_t(b[p++])<<(8*i);return true;}
bool g64(const std::vector<uint8_t>&b,size_t&p,uint64_t&v){if(p>b.size()||b.size()-p<8)return false;v=0;for(int i=0;i<8;++i)v|=uint64_t(b[p++])<<(8*i);return true;}
void fail(Diagnostics&d,const char*c){d.add(c,{},c);}
bool read(const std::filesystem::path&p,std::vector<uint8_t>&b){FILE*f=std::fopen(p.string().c_str(),"rb");if(!f)return false;std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::fseek(f,0,SEEK_SET);if(n<0){std::fclose(f);return false;}b.resize(size_t(n));bool ok=std::fread(b.data(),1,b.size(),f)==b.size();std::fclose(f);return ok;}
bool write(const std::filesystem::path&p,const std::vector<uint8_t>&b){FILE*f=std::fopen(p.string().c_str(),"wb");if(!f)return false;bool ok=std::fwrite(b.data(),1,b.size(),f)==b.size()&&std::fflush(f)==0;std::fclose(f);return ok;}
bool checksum_part(const std::filesystem::path&p,uint64_t&out){std::vector<uint8_t>b;if(!read(p,b)||b.size()<40)return false;out=fnv(b.data()+40,b.size()-40);return true;}
std::filesystem::path tmp(const std::filesystem::path&p){return p.string()+".bundle.tmp";}
bool make_manifest(const BundleIdentity&i,std::vector<uint8_t>&b){if(i.lods.size()>UINT32_MAX)return false;b.insert(b.end(),{'M','A','C','M'});u32(b,1);u64(b,i.resolved_hash);u64(b,i.nonce.high);u64(b,i.nonce.low);u64(b,i.part_body_checksum);u64(b,i.anim_body_checksum);u32(b,i.part_format_version);u32(b,i.animation_schema_version);u32(b,i.animation_bake_epoch);u32(b,i.target_abi_tag);u32(b,i.ozz_tag_hash);u32(b,uint32_t(i.lods.size()));for(auto&l:i.lods){u64(b,l.indexed_vertex_signature);u32(b,l.vertex_count);u32(b,l.influence_count);}u64(b,fnv(b.data(),b.size()));return true;}
bool parse_manifest(const std::filesystem::path&p,BundleIdentity&i){std::vector<uint8_t>b;if(!read(p,b)||b.size()<80||std::memcmp(b.data(),"MACM",4))return false;size_t x=b.size()-8;uint64_t sum=0;if(!g64(b,x,sum)||sum!=fnv(b.data(),b.size()-8))return false;x=4;uint32_t v=0,n=0;if(!g32(b,x,v)||v!=1||!g64(b,x,i.resolved_hash)||!g64(b,x,i.nonce.high)||!g64(b,x,i.nonce.low)||!g64(b,x,i.part_body_checksum)||!g64(b,x,i.anim_body_checksum)||!g32(b,x,i.part_format_version)||!g32(b,x,i.animation_schema_version)||!g32(b,x,i.animation_bake_epoch)||!g32(b,x,i.target_abi_tag)||!g32(b,x,i.ozz_tag_hash)||!g32(b,x,n)||n>(b.size()-x-8)/16)return false;i.lods.resize(n);for(auto&l:i.lods)if(!g64(b,x,l.indexed_vertex_signature)||!g32(b,x,l.vertex_count)||!g32(b,x,l.influence_count))return false;return x==b.size()-8;}
}

bool publish_animation_bundle(const BundleCandidates& c,const BundleIdentity& i,Diagnostics& d){
    AnimAsset a; uint64_t pc=0; std::optional<part_asset::PartAnimationLink> link;
    if(!load_anim(c.anim_candidate,a,d)||a.resolved_hash!=i.resolved_hash||!(a.nonce==i.nonce)||anim_body_checksum(a)!=i.anim_body_checksum||!checksum_part(c.part_candidate,pc)||pc!=i.part_body_checksum||!part_asset::load_animation_link(c.part_candidate.string(),i.resolved_hash,link)||!link||link->nonce_high!=i.nonce.high||link->nonce_low!=i.nonce.low){fail(d,"bundle.candidate");return false;}
    std::error_code ec;std::filesystem::create_directories(c.cache_root/"parts",ec);if(ec){fail(d,"bundle.directory");return false;}
    auto part=c.cache_root/part_asset::cache_path_resolved(i.resolved_hash);auto anim=cache_path_anim(c.cache_root,i.resolved_hash);auto manifest=cache_path_anim_commit(c.cache_root,i.resolved_hash);std::vector<uint8_t>m;
    if(!make_manifest(i,m)||!write(tmp(manifest),m)||!part_asset::replace_file_atomic(c.part_candidate.string(),part.string())||!part_asset::replace_file_atomic(c.anim_candidate.string(),anim.string())||!part_asset::replace_file_atomic(tmp(manifest).string(),manifest.string())){fail(d,"bundle.publish");return false;}return true;
}
bool load_committed_animation_bundle(const std::filesystem::path& root,uint64_t hash,BLASManager&,AnimAsset&out,Diagnostics&d){
    BundleIdentity i;
    if (!parse_manifest(cache_path_anim_commit(root, hash), i) || i.resolved_hash != hash) { fail(d, "bundle.manifest"); return false; }
    auto part = root / part_asset::cache_path_resolved(hash);
    uint64_t pc = 0;
    if (!checksum_part(part, pc) || pc != i.part_body_checksum) { fail(d, "bundle.part_checksum"); return false; }
    std::optional<part_asset::PartAnimationLink> link;
    if (!part_asset::load_animation_link(part.string(), hash, link) || !link ||
        link->nonce_high != i.nonce.high || link->nonce_low != i.nonce.low) { fail(d, "bundle.link"); return false; }
    AnimAsset a;
    if (!load_anim(cache_path_anim(root, hash), a, d) || a.resolved_hash != hash ||
        !(a.nonce == i.nonce) || a.target_abi_tag != i.target_abi_tag ||
        a.ozz_tag_hash != i.ozz_tag_hash || anim_body_checksum(a) != i.anim_body_checksum) { fail(d, "bundle.anim"); return false; }
    out = std::move(a);
    return true;
}
} // namespace matter::animation
