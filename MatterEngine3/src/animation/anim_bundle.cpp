#include "animation/anim_bundle.h"
#include "animation/animation_binding_bake.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_set>
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace matter::animation {
namespace {
bool g_test_replace_legacy_lock_directory_once = false;
uint64_t fnv(const uint8_t* p, size_t n) { uint64_t h=1469598103934665603ull; for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;} return h; }
void u32(std::vector<uint8_t>& b,uint32_t v){for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));}
void u64(std::vector<uint8_t>& b,uint64_t v){for(int i=0;i<8;++i)b.push_back(uint8_t(v>>(8*i)));}
bool g32(const std::vector<uint8_t>&b,size_t&p,uint32_t&v){if(p>b.size()||b.size()-p<4)return false;v=0;for(int i=0;i<4;++i)v|=uint32_t(b[p++])<<(8*i);return true;}
bool g64(const std::vector<uint8_t>&b,size_t&p,uint64_t&v){if(p>b.size()||b.size()-p<8)return false;v=0;for(int i=0;i<8;++i)v|=uint64_t(b[p++])<<(8*i);return true;}
void fail(Diagnostics&d,const char*c){d.add(c,{},c);}
bool valid_lods(const std::vector<LodBindingSignature>& lods) { if(lods.size()>64)return false; std::unordered_set<uint64_t> identities; for(const auto& lod:lods){const uint64_t max_influences=uint64_t(lod.vertex_count)*4u; if(lod.indexed_vertex_signature==0||lod.vertex_count==0||lod.influence_count==0||uint64_t(lod.influence_count)>max_influences||!identities.insert(lod.indexed_vertex_signature).second)return false;}return true; }
bool part_matches_binding(const BLASManager& blas, const part_asset::LodLevels& lods,
                          const BindingBake& binding) {
    // Rigid segments and attachments are typed animation declarations; they
    // do not carry skinned vertex data to compare against the Part's indexed
    // geometry.  The caller has already fully validated the Part and MANM
    // payloads, so semantic geometry matching applies only when a skin LOD
    // is present.
    if (binding.lods.empty()) return true;
    std::vector<std::vector<uint32_t>> levels;
    const auto& entries = blas.get_entries();
    if (lods.empty()) {
        if (entries.empty()) return false;
        levels.emplace_back();
        levels.back().reserve(entries.size());
        for (uint32_t index = 0; index < entries.size(); ++index) levels.back().push_back(index);
    } else {
        if (lods.size() > 64) return false;
        levels.reserve(lods.size());
        for (const auto& lod : lods) {
            if (lod.blas_indices.empty()) return false;
            std::unordered_set<uint32_t> seen;
            for (uint32_t index : lod.blas_indices)
                if (index >= entries.size() || !entries[index] || !seen.insert(index).second) return false;
            levels.push_back(lod.blas_indices);
        }
    }
    if (levels.size() != binding.lods.size()) return false;
    for (size_t level = 0; level < levels.size(); ++level) {
        std::vector<Tri> triangles;
        std::vector<TriEx> triex;
        bool all_have_triex = true;
        for (uint32_t index : levels[level]) {
            const auto& entry = entries[index];
            const TriEx* extra = entry->tri_extra.size() == entry->triangles.size()
                                 ? entry->tri_extra.data() : entry->mesh->triEx;
            if (!extra) all_have_triex = false;
            triangles.insert(triangles.end(), entry->triangles.begin(), entry->triangles.end());
            if (extra) triex.insert(triex.end(), extra, extra + entry->triangles.size());
        }
        // The existing raster path has one coherent TriEx mode per indexed
        // stream.  A mixed stream has no exact shared representation yet, so
        // reject it rather than accept a binding against different semantics.
        if (triangles.empty() || (!all_have_triex && !triex.empty())) return false;
        const auto geometry = viewer::build_indexed_part_geometry(
            triangles.data(), all_have_triex ? triex.data() : nullptr,
            static_cast<int>(triangles.size()));
        const auto& baked = binding.lods[level];
        if (geometry.vertex_count <= 0 || baked.vertex_count != static_cast<uint32_t>(geometry.vertex_count) ||
            baked.indexed_vertex_signature != viewer::indexed_part_geometry_signature(geometry, static_cast<uint32_t>(level)))
            return false;
    }
    return true;
}
bool read(const std::filesystem::path&p,std::vector<uint8_t>&b){FILE*f=std::fopen(p.string().c_str(),"rb");if(!f)return false;std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::fseek(f,0,SEEK_SET);if(n<0){std::fclose(f);return false;}b.resize(size_t(n));bool ok=std::fread(b.data(),1,b.size(),f)==b.size();std::fclose(f);return ok;}
bool durable_flush(FILE* f) { if(std::fflush(f)!=0)return false;
#ifdef _WIN32
return _commit(_fileno(f))==0;
#else
return fsync(fileno(f))==0;
#endif
}
bool sync_parent(const std::filesystem::path& p) {
#ifdef _WIN32
    (void)p; return true;
#else
    auto parent=p.parent_path(); if(parent.empty())parent=".";
    const int fd=open(parent.string().c_str(),O_RDONLY|O_DIRECTORY); if(fd<0)return false;
    const bool ok=fsync(fd)==0; const int closed=close(fd); return ok&&closed==0;
#endif
}
bool flush_existing(const std::filesystem::path& p) { FILE* f=std::fopen(p.string().c_str(),"rb");if(!f)return false;bool ok=durable_flush(f);bool closed=std::fclose(f)==0;return ok&&closed; }
bool write(const std::filesystem::path&p,const std::vector<uint8_t>&b){FILE*f=std::fopen(p.string().c_str(),"wb");if(!f)return false;bool ok=std::fwrite(b.data(),1,b.size(),f)==b.size()&&durable_flush(f);bool closed=std::fclose(f)==0;return ok&&closed;}
bool checksum_part(const std::filesystem::path&p,uint64_t&out){std::vector<uint8_t>b;if(!read(p,b)||b.size()<40)return false;out=fnv(b.data()+40,b.size()-40);return true;}
std::string nonce_suffix(const BuildNonce& n) { char out[40]; std::snprintf(out,sizeof out,".%016llx%016llx",(unsigned long long)n.high,(unsigned long long)n.low); return out; }
std::filesystem::path tmp(const std::filesystem::path&p,const BuildNonce&n){return p.string()+nonce_suffix(n)+".tmp";}
std::filesystem::path backup(const std::filesystem::path&p,const BuildNonce&n){return p.string()+nonce_suffix(n)+".backup";}
bool backup_existing(const std::filesystem::path& path,const BuildNonce& n, bool& existed) { std::error_code ec; existed=std::filesystem::exists(path,ec); if(ec||!existed)return !ec; const auto copy=backup(path,n); return std::filesystem::copy_file(path,copy,std::filesystem::copy_options::overwrite_existing,ec)&&!ec&&flush_existing(copy)&&sync_parent(copy); }
bool rollback(const std::filesystem::path& path,const BuildNonce& n, bool existed) { std::error_code ec; if(existed)return part_asset::replace_file_atomic_detailed(backup(path,n).string(),path.string())==part_asset::FileReplaceOutcome::ReplacedDurable; const bool present=std::filesystem::exists(path,ec); if(ec)return false; return !present || (std::filesystem::remove(path,ec)&&!ec&&sync_parent(path)); }
bool discard_backup(const std::filesystem::path& path,const BuildNonce& n) { std::error_code ec; const auto p=backup(path,n); const bool present=std::filesystem::exists(p,ec); if(ec)return false; return !present || (std::filesystem::remove(p,ec)&&!ec&&sync_parent(p)); }
bool discard_backups(const std::filesystem::path& part,const std::filesystem::path& anim,const std::filesystem::path& manifest,const BuildNonce& n) { return discard_backup(part,n)&&discard_backup(anim,n)&&discard_backup(manifest,n); }
bool discard_temporary(const std::filesystem::path& p) { std::error_code ec; const bool present=std::filesystem::exists(p,ec); if(ec)return false; return !present || (std::filesystem::remove(p,ec)&&!ec&&sync_parent(p)); }
struct PublicationLock {
    std::filesystem::path path;
    bool held = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool remove_empty_directory_only() {
#ifdef _WIN32
        return RemoveDirectoryA(path.string().c_str()) != 0;
#else
        return rmdir(path.string().c_str()) == 0;
#endif
    }
    bool migrate_legacy_empty_directory() {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (ec) return ec == std::errc::no_such_file_or_directory;
        if (!std::filesystem::exists(status)) return true;
        if (!std::filesystem::is_directory(status))
            return std::filesystem::is_regular_file(status);
        // Only the old directory sentinel can be removed, and only while it
        // is empty.  Any race or non-empty directory fails closed.
        if (g_test_replace_legacy_lock_directory_once) {
            g_test_replace_legacy_lock_directory_once = false;
            if (!remove_empty_directory_only()) return false;
            FILE* replacement = std::fopen(path.string().c_str(), "wb");
            if (!replacement) return false;
            const bool closed = std::fclose(replacement) == 0;
            return false && closed;
        }
        return remove_empty_directory_only();
    }
    bool acquire(const std::filesystem::path& target) {
        path = target.string() + ".lock";
        if (!migrate_legacy_empty_directory()) return false;
#ifdef _WIN32
        handle = CreateFileA(path.string().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        held = handle != INVALID_HANDLE_VALUE;
#else
        fd = open(path.string().c_str(), O_CREAT | O_RDWR, 0600);
        held = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
        if (!held && fd >= 0) { close(fd); fd = -1; }
#endif
        return held;
    }
    bool release() {
        if (!held) return true;
        bool closed = false;
#ifdef _WIN32
        closed = CloseHandle(handle) != 0; handle = INVALID_HANDLE_VALUE;
#else
        closed = close(fd) == 0; fd = -1;
#endif
        held = false;
        // Keep the regular lock file inode permanently.  Removing it after
        // close races another opener which may still hold the old inode.
        return closed;
    }
    ~PublicationLock() { release(); }
};
std::unique_ptr<PublicationLock> g_test_held_publication_lock;
bool make_manifest(const BundleIdentity&i,std::vector<uint8_t>&b){if(i.lods.size()>UINT32_MAX||!valid_lods(i.lods))return false;b.insert(b.end(),{'M','A','C','M'});u32(b,1);u64(b,i.resolved_hash);u64(b,i.nonce.high);u64(b,i.nonce.low);u64(b,i.part_body_checksum);u64(b,i.anim_body_checksum);u32(b,i.part_format_version);u32(b,i.animation_schema_version);u32(b,i.animation_bake_epoch);u32(b,i.target_abi_tag);u32(b,i.ozz_tag_hash);u32(b,i.compiler_identifier);u32(b,uint32_t(i.lods.size()));for(auto&l:i.lods){u64(b,l.indexed_vertex_signature);u32(b,l.vertex_count);u32(b,l.influence_count);}u64(b,fnv(b.data(),b.size()));return true;}
bool parse_manifest(const std::filesystem::path&p,BundleIdentity&i){std::vector<uint8_t>b;if(!read(p,b)||b.size()<84||std::memcmp(b.data(),"MACM",4))return false;size_t x=b.size()-8;uint64_t sum=0;if(!g64(b,x,sum)||sum!=fnv(b.data(),b.size()-8))return false;x=4;uint32_t v=0,n=0;if(!g32(b,x,v)||v!=1||!g64(b,x,i.resolved_hash)||!g64(b,x,i.nonce.high)||!g64(b,x,i.nonce.low)||!g64(b,x,i.part_body_checksum)||!g64(b,x,i.anim_body_checksum)||!g32(b,x,i.part_format_version)||!g32(b,x,i.animation_schema_version)||!g32(b,x,i.animation_bake_epoch)||!g32(b,x,i.target_abi_tag)||!g32(b,x,i.ozz_tag_hash)||!g32(b,x,i.compiler_identifier)||!g32(b,x,n)||n>64||n>(b.size()-x-8)/16)return false;i.lods.resize(n);for(auto&l:i.lods)if(!g64(b,x,l.indexed_vertex_signature)||!g32(b,x,l.vertex_count)||!g32(b,x,l.influence_count))return false;return x==b.size()-8&&valid_lods(i.lods);}
}

bool publish_animation_bundle(const BundleCandidates& c,const BundleIdentity& i,Diagnostics& d){
    AnimAsset a; uint64_t pc=0; std::optional<part_asset::PartAnimationLink> link;
    BLASManager candidate_blas; TLASManager candidate_tlas(1 << 20);
    std::vector<part_asset::ChildInstance> candidate_children; part_asset::LodLevels candidate_lods;
    std::vector<part_asset::VolumeEmitter> candidate_emitters;
    BindingBake binding;
    if(i.part_format_version!=part_asset::kFormatVersionV2||i.animation_schema_version!=kAnimationSchemaVersion||i.animation_bake_epoch!=kAnimationBakeEpoch||i.compiler_identifier!=kAnimationCompilerIdentifier||i.target_abi_tag!=kAnimationTargetAbiTag||i.ozz_tag_hash!=kAnimationOzzTagHash||(i.nonce.high==0&&i.nonce.low==0)||!load_anim(c.anim_candidate,a,d)||!get_anim_binding_bake(a,binding)||!manifest_matches_binding(i.lods,binding)||a.resolved_hash!=i.resolved_hash||!(a.nonce==i.nonce)||a.target_abi_tag!=kAnimationTargetAbiTag||a.ozz_tag_hash!=kAnimationOzzTagHash||a.target_abi_tag!=i.target_abi_tag||a.ozz_tag_hash!=i.ozz_tag_hash||anim_body_checksum(a)!=i.anim_body_checksum||!checksum_part(c.part_candidate,pc)||pc!=i.part_body_checksum||!part_asset::load_v2(c.part_candidate.string(),i.resolved_hash,candidate_blas,candidate_tlas,candidate_children,candidate_lods,candidate_emitters,link)||!part_matches_binding(candidate_blas,candidate_lods,binding)||!link||link->nonce_high!=i.nonce.high||link->nonce_low!=i.nonce.low){fail(d,"bundle.candidate");return false;}
    std::error_code ec;std::filesystem::create_directories(c.cache_root/"parts",ec);if(ec){fail(d,"bundle.directory");return false;}
    auto part=c.cache_root/part_asset::cache_path_resolved(i.resolved_hash);auto anim=cache_path_anim(c.cache_root,i.resolved_hash);auto manifest=cache_path_anim_commit(c.cache_root,i.resolved_hash);std::vector<uint8_t>m;
    if(c.test_hold_publication_lock){
        if(g_test_held_publication_lock){fail(d,"bundle.lock");return false;}
        auto held=std::make_unique<PublicationLock>();
        if(!held->acquire(manifest)){fail(d,"bundle.lock");return false;}
        g_test_held_publication_lock=std::move(held);
        fail(d,"bundle.injected");return false;
    }
    PublicationLock lock; if(!lock.acquire(manifest)){fail(d,"bundle.lock");return false;}
    bool had_part=false,had_anim=false,had_manifest=false;
    const auto cleanup = [&] { return discard_backups(part,anim,manifest,i.nonce)&&discard_temporary(tmp(manifest,i.nonce)); };
    if(!make_manifest(i,m)||!write(tmp(manifest,i.nonce),m)||!sync_parent(tmp(manifest,i.nonce))||!backup_existing(part,i.nonce,had_part)||!backup_existing(anim,i.nonce,had_anim)||!backup_existing(manifest,i.nonce,had_manifest)){ if(!cleanup())fail(d,"bundle.cleanup"); fail(d,"bundle.publish");return false;}
    if(c.test_fail_before_replace==1){if(!cleanup())fail(d,"bundle.cleanup");fail(d,"bundle.injected");return false;}
    if(c.test_fail_after_replace_sync==1)part_asset::set_replace_file_atomic_test_post_rename_failure_once();
    const auto part_result=part_asset::replace_file_atomic_detailed(c.part_candidate.string(),part.string());
    if(part_result!=part_asset::FileReplaceOutcome::ReplacedDurable){const bool restored=part_result==part_asset::FileReplaceOutcome::NotReplaced||rollback(part,i.nonce,had_part);if(!restored)fail(d,"bundle.rollback");else if(!cleanup())fail(d,"bundle.cleanup");fail(d,"bundle.publish");return false;}
    if(c.test_fail_before_replace==2){const bool restored=rollback(part,i.nonce,had_part);if(!restored)fail(d,"bundle.rollback");else if(!cleanup())fail(d,"bundle.cleanup");fail(d,"bundle.injected");return false;}
    if(c.test_fail_after_replace_sync==2)part_asset::set_replace_file_atomic_test_post_rename_failure_once();
    const auto anim_result=part_asset::replace_file_atomic_detailed(c.anim_candidate.string(),anim.string());
    if(anim_result!=part_asset::FileReplaceOutcome::ReplacedDurable){const bool restored=(anim_result==part_asset::FileReplaceOutcome::NotReplaced||rollback(anim,i.nonce,had_anim))&&rollback(part,i.nonce,had_part);if(!restored)fail(d,"bundle.rollback");else if(!cleanup())fail(d,"bundle.cleanup");fail(d,"bundle.publish");return false;}
    if(c.test_fail_before_replace==3){const bool restored=rollback(anim,i.nonce,had_anim)&&rollback(part,i.nonce,had_part);if(!restored)fail(d,"bundle.rollback");else if(!cleanup())fail(d,"bundle.cleanup");fail(d,"bundle.injected");return false;}
    if(c.test_fail_after_replace_sync==3)part_asset::set_replace_file_atomic_test_post_rename_failure_once();
    const auto manifest_result=part_asset::replace_file_atomic_detailed(tmp(manifest,i.nonce).string(),manifest.string());
    if(manifest_result!=part_asset::FileReplaceOutcome::ReplacedDurable){const bool restored=(manifest_result==part_asset::FileReplaceOutcome::NotReplaced||rollback(manifest,i.nonce,had_manifest))&&rollback(anim,i.nonce,had_anim)&&rollback(part,i.nonce,had_part);if(!restored)fail(d,"bundle.rollback");else if(!cleanup())fail(d,"bundle.cleanup");fail(d,"bundle.publish");return false;}
    if(!cleanup()||!lock.release()){fail(d,"bundle.cleanup");return false;}return true;
}
void release_animation_bundle_test_lock() { if(g_test_held_publication_lock){g_test_held_publication_lock->release();g_test_held_publication_lock.reset();} }
void set_animation_bundle_test_replace_legacy_lock_directory_once() { g_test_replace_legacy_lock_directory_once = true; }
bool load_committed_animation_bundle(const std::filesystem::path& root,uint64_t hash,BLASManager& blas,AnimAsset&out,Diagnostics&d){
    BundleIdentity i;
    if (!parse_manifest(cache_path_anim_commit(root, hash), i) || i.resolved_hash != hash ||
        i.part_format_version != part_asset::kFormatVersionV2 ||
        i.animation_schema_version != kAnimationSchemaVersion ||
        i.animation_bake_epoch != kAnimationBakeEpoch || i.compiler_identifier != kAnimationCompilerIdentifier ||
        i.target_abi_tag != kAnimationTargetAbiTag || i.ozz_tag_hash != kAnimationOzzTagHash ||
        (i.nonce.high == 0 && i.nonce.low == 0)) { fail(d, "bundle.manifest"); return false; }
    auto part = root / part_asset::cache_path_resolved(hash);
    uint64_t pc = 0;
    if (!checksum_part(part, pc) || pc != i.part_body_checksum) { fail(d, "bundle.part_checksum"); return false; }
    BLASManager checked_blas; TLASManager checked_tlas(1 << 20);
    std::vector<part_asset::ChildInstance> checked_children; part_asset::LodLevels checked_lods;
    std::vector<part_asset::VolumeEmitter> checked_emitters; std::optional<part_asset::PartAnimationLink> link;
    if (!part_asset::load_v2(part.string(), hash, checked_blas, checked_tlas, checked_children,
                             checked_lods, checked_emitters, link) || !link ||
        link->nonce_high != i.nonce.high || link->nonce_low != i.nonce.low) { fail(d, "bundle.link"); return false; }
    AnimAsset a;
    if (!load_anim(cache_path_anim(root, hash), a, d) || a.resolved_hash != hash ||
        !(a.nonce == i.nonce) || a.target_abi_tag != kAnimationTargetAbiTag ||
        a.ozz_tag_hash != kAnimationOzzTagHash || a.target_abi_tag != i.target_abi_tag ||
        a.ozz_tag_hash != i.ozz_tag_hash || anim_body_checksum(a) != i.anim_body_checksum) { fail(d, "bundle.anim"); return false; }
    BindingBake binding;
    if (!get_anim_binding_bake(a, binding) || !manifest_matches_binding(i.lods, binding) ||
        !part_matches_binding(checked_blas, checked_lods, binding)) {
        fail(d, "bundle.binding"); return false;
    }
    // Validation happens entirely in candidate state.  Only a fully coherent
    // sibling set is allowed to replace the caller's last-good part state.
    blas = std::move(checked_blas);
    out = std::move(a);
    return true;
}
} // namespace matter::animation
