#include "animation/anim_asset.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace matter::animation {
namespace {
constexpr size_t kHeaderBytes = 68;
constexpr size_t kSectionBytes = 20;
uint64_t fnv(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i=0;i<n;++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }
void u32(std::vector<uint8_t>& b, uint32_t v) { for (int i=0;i<4;++i) b.push_back(uint8_t(v >> (i*8))); }
void u64(std::vector<uint8_t>& b, uint64_t v) { for (int i=0;i<8;++i) b.push_back(uint8_t(v >> (i*8))); }
bool get32(const std::vector<uint8_t>& b, size_t& p, uint32_t& v) { if (p > b.size() || b.size()-p < 4) return false; v=0; for(int i=0;i<4;++i) v|=uint32_t(b[p++])<<(i*8); return true; }
bool get64(const std::vector<uint8_t>& b, size_t& p, uint64_t& v) { if (p > b.size() || b.size()-p < 8) return false; v=0; for(int i=0;i<8;++i) v|=uint64_t(b[p++])<<(i*8); return true; }
void fail(Diagnostics& d, const char* code) { d.add(code, {}, code); }
bool plus_ok(uint64_t a, uint64_t b, uint64_t& out) { if (a > std::numeric_limits<uint64_t>::max()-b) return false; out=a+b; return true; }
bool durable_flush(FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
bool required_sections(const std::vector<AnimSection>& sections) {
    uint32_t seen = 0;
    for (const auto& section : sections) {
        const uint32_t kind = static_cast<uint32_t>(section.kind);
        if (kind == 0 || kind > 10) return false;
        const uint32_t bit = 1u << (kind - 1);
        if ((seen & bit) != 0) return false;
        seen |= bit;
    }
    return seen == ((1u << 10) - 1u);
}
}

BuildNonce generate_build_nonce() {
    std::random_device entropy;
    BuildNonce nonce{(uint64_t(entropy()) << 32) ^ entropy(),
                     (uint64_t(entropy()) << 32) ^ entropy()};
    if (nonce.high == 0 && nonce.low == 0) nonce.low = 1;
    return nonce;
}

std::filesystem::path cache_path_anim(const std::filesystem::path& root, uint64_t h) { char x[32]; std::snprintf(x,sizeof x,"%016llx.anim",static_cast<unsigned long long>(h)); return root / "parts" / x; }
std::filesystem::path cache_path_anim_commit(const std::filesystem::path& root, uint64_t h) { auto p=cache_path_anim(root,h); return p.string()+".commit"; }

uint64_t anim_body_checksum(const AnimAsset& a) { std::vector<uint8_t> b; for (const auto& s:a.sections) { u32(b,uint32_t(s.kind)); u64(b,s.bytes.size()); b.insert(b.end(),s.bytes.begin(),s.bytes.end()); } return fnv(b.data(),b.size()); }

bool save_anim_candidate(const AnimAsset& a, const std::filesystem::path& path, Diagnostics& d) {
    if (a.sections.size() > UINT32_MAX || !required_sections(a.sections)) { fail(d,"anim.sections"); return false; }
    std::vector<uint8_t> table, payload;
    uint64_t off = kHeaderBytes + uint64_t(a.sections.size()) * kSectionBytes;
    for (const auto& s : a.sections) {
        uint64_t end=0; if (!plus_ok(off,s.bytes.size(),end)) { fail(d,"anim.offset_overflow"); return false; }
        u32(table,uint32_t(s.kind)); u64(table,off); u64(table,s.bytes.size());
        payload.insert(payload.end(),s.bytes.begin(),s.bytes.end()); off=end;
    }
    std::vector<uint8_t> out; out.insert(out.end(),{'M','A','N','M'}); u32(out,kAnimFormatVersion); u32(out,kAnimationSchemaVersion); u32(out,kAnimationBakeEpoch);
    u64(out,a.resolved_hash); u64(out,a.nonce.high); u64(out,a.nonce.low); u32(out,a.target_abi_tag); u32(out,a.ozz_tag_hash); u32(out,uint32_t(a.sections.size())); u64(out,kHeaderBytes); u64(out,0);
    if (out.size()!=kHeaderBytes) { fail(d,"anim.header_layout"); return false; }
    out.insert(out.end(),table.begin(),table.end()); out.insert(out.end(),payload.begin(),payload.end());
    uint64_t full = 1469598103934665603ull;
    for (size_t i = 0; i < out.size(); ++i) {
        const uint8_t byte = (i >= 60 && i < 68) ? 0 : out[i];
        full ^= byte; full *= 1099511628211ull;
    }
    for (int i = 0; i < 8; ++i) out[60 + i] = uint8_t(full >> (8 * i));
    std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec); FILE* f=std::fopen(path.string().c_str(),"wb"); if(!f){fail(d,"anim.write");return false;} const bool ok=std::fwrite(out.data(),1,out.size(),f)==out.size() && durable_flush(f); const bool closed=std::fclose(f)==0; if(!ok || !closed) fail(d,"anim.write"); return ok && closed;
}

bool load_anim(const std::filesystem::path& path, AnimAsset& a, Diagnostics& d) {
    a={}; FILE* f=std::fopen(path.string().c_str(),"rb"); if(!f){fail(d,"anim.open");return false;} std::fseek(f,0,SEEK_END); long size=std::ftell(f); std::fseek(f,0,SEEK_SET); if(size < long(kHeaderBytes)){std::fclose(f);fail(d,"anim.truncated");return false;} std::vector<uint8_t> b(static_cast<size_t>(size)); bool read=std::fread(b.data(),1,b.size(),f)==b.size();std::fclose(f);if(!read){fail(d,"anim.read");return false;}
    if(std::memcmp(b.data(),"MANM",4)!=0){fail(d,"anim.magic");return false;} size_t p=4; uint32_t format=0,schema=0,epoch=0,abi=0,ozz=0,count=0; uint64_t table=0,checksum=0;
    if(!get32(b,p,format)||!get32(b,p,schema)||!get32(b,p,epoch)||!get64(b,p,a.resolved_hash)||!get64(b,p,a.nonce.high)||!get64(b,p,a.nonce.low)||!get32(b,p,abi)||!get32(b,p,ozz)||!get32(b,p,count)||!get64(b,p,table)||!get64(b,p,checksum)){fail(d,"anim.truncated");return false;}
    if(format!=kAnimFormatVersion||schema!=kAnimationSchemaVersion||epoch!=kAnimationBakeEpoch){fail(d,"anim.version");return false;} a.target_abi_tag=abi;a.ozz_tag_hash=ozz;
    uint64_t table_end=0; if(table!=kHeaderBytes || !plus_ok(table,uint64_t(count)*kSectionBytes,table_end) || table_end>b.size()){fail(d,"anim.section_table");return false;}
    struct Entry { uint32_t kind; uint64_t off,len; }; std::vector<Entry> es; p=size_t(table); for(uint32_t i=0;i<count;++i){Entry e{};if(!get32(b,p,e.kind)||!get64(b,p,e.off)||!get64(b,p,e.len)){fail(d,"anim.section_table");return false;}uint64_t end=0;if(!plus_ok(e.off,e.len,end)||e.off<table_end||end>b.size()){fail(d,"anim.section_range");return false;}es.push_back(e);}
    std::sort(es.begin(),es.end(),[](const Entry&x,const Entry&y){return x.off<y.off;}); uint64_t prior=table_end; for(const auto&e:es){if(e.off<prior){fail(d,"anim.section_overlap");return false;}prior=e.off+e.len;}
    uint64_t full = 1469598103934665603ull;
    for (size_t i = 0; i < b.size(); ++i) {
        const uint8_t byte = (i >= 60 && i < 68) ? 0 : b[i];
        full ^= byte; full *= 1099511628211ull;
    }
    if (full != checksum) { fail(d,"anim.checksum"); return false; }
    for (const auto& e : es) {
        a.sections.push_back({AnimSectionKind(e.kind),
                              std::vector<uint8_t>(b.begin() + size_t(e.off),
                                                   b.begin() + size_t(e.off + e.len))});
    }
    if (!required_sections(a.sections)) { a = {}; fail(d, "anim.sections"); return false; }
    return true;
}
} // namespace matter::animation
