#include "../include/imposter_asset.h"
#include "../include/mesh_simplifier.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <sys/stat.h>

namespace {
template <class T> void put(std::vector<uint8_t>& b, const T& v){
    const uint8_t* p=reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(),p,p+sizeof(T));
}
void put_bytes(std::vector<uint8_t>& b, const void* d, size_t n){
    const uint8_t* p=static_cast<const uint8_t*>(d); b.insert(b.end(),p,p+n);
}
void ensure_parent_dir(const std::string& path){
    auto pos=path.find_last_of('/'); if(pos==std::string::npos) return;
#ifdef _WIN32
    mkdir(path.substr(0,pos).c_str());
#else
    mkdir(path.substr(0,pos).c_str(), 0755);
#endif
}
struct Reader {
    const uint8_t* p; const uint8_t* end; bool ok=true;
    template <class T> T get(){ T v{}; if(p+sizeof(T)>end){ok=false;return v;} std::memcpy(&v,p,sizeof(T)); p+=sizeof(T); return v; }
    const uint8_t* take(size_t n){ if(p+n>end){ok=false;return nullptr;} const uint8_t* r=p; p+=n; return r; }
};
} // namespace

namespace imposter_asset {

uint64_t compute_imp_hash(const ImpGenParams& p) {
    return part_asset::fnv1a64(&p, sizeof(p)) ^ static_cast<uint64_t>(kFormatVersion);
}

std::string cache_path(uint64_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("imposters/") + buf + ".imp";
}

bool save(const std::string& path, const ImposterAsset& a, uint64_t imp_hash) {
    std::vector<uint8_t> body;
    put_bytes(body, a.bounds_min, 3*sizeof(float));
    put_bytes(body, a.bounds_max, 3*sizeof(float));
    put<float>(body, a.max_disp);
    put<float>(body, a.parallax_radius);
    put<uint32_t>(body, a.atlas_w);
    put<uint32_t>(body, a.atlas_h);
    put<uint32_t>(body, static_cast<uint32_t>(a.disp_bits));
    put<uint32_t>(body, static_cast<uint32_t>(a.verts.size()));
    put_bytes(body, a.verts.data(), a.verts.size()*sizeof(CageVert));
    put<uint32_t>(body, static_cast<uint32_t>(a.tris.size()));
    put_bytes(body, a.tris.data(), a.tris.size()*sizeof(CageTri));
    put<uint32_t>(body, static_cast<uint32_t>(a.disp.size()));
    put_bytes(body, a.disp.data(), a.disp.size());
    put<uint32_t>(body, static_cast<uint32_t>(a.color.size()));
    put_bytes(body, a.color.data(), a.color.size());

    const uint64_t content_hash = part_asset::fnv1a64(body.data(), body.size());
    std::vector<uint8_t> head;
    put<uint32_t>(head, kMagic);
    put<uint32_t>(head, kFormatVersion);
    put<uint64_t>(head, imp_hash);
    put<uint64_t>(head, a.source_part_hash);
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(CageVert)));
    put<uint32_t>(head, static_cast<uint32_t>(sizeof(CageTri)));
    put<uint64_t>(head, content_hash);

    ensure_parent_dir(path);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head.data(),1,head.size(),f)==head.size() &&
              std::fwrite(body.data(),1,body.size(),f)==body.size();
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool load(const std::string& path, uint64_t expected_imp_hash,
          uint64_t expected_source_hash, ImposterAsset& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f,0,SEEK_END); long sz=std::ftell(f); std::fseek(f,0,SEEK_SET);
    if (sz < 44) { std::fclose(f); return false; }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    bool read_ok = std::fread(buf.data(),1,buf.size(),f)==buf.size();
    std::fclose(f);
    if (!read_ok) return false;

    Reader r{ buf.data(), buf.data()+buf.size() };
    const uint32_t magic   = r.get<uint32_t>();
    const uint32_t version = r.get<uint32_t>();
    const uint64_t ihash   = r.get<uint64_t>();
    const uint64_t shash   = r.get<uint64_t>();
    const uint32_t s_vert  = r.get<uint32_t>();
    const uint32_t s_tri   = r.get<uint32_t>();
    const uint64_t content = r.get<uint64_t>();
    if (!r.ok) return false;
    if (magic != kMagic)               return false;
    if (version != kFormatVersion)     return false;
    if (s_vert != sizeof(CageVert))    return false;
    if (s_tri  != sizeof(CageTri))     return false;
    if (ihash != expected_imp_hash)    return false;
    if (shash != expected_source_hash) return false;
    if (part_asset::fnv1a64(r.p, static_cast<size_t>(r.end - r.p)) != content) return false;

    out = ImposterAsset{};
    out.source_part_hash = shash;
    const uint8_t* bmin = r.take(3*sizeof(float));
    const uint8_t* bmax = r.take(3*sizeof(float));
    if (!r.ok) return false;
    std::memcpy(out.bounds_min, bmin, 3*sizeof(float));
    std::memcpy(out.bounds_max, bmax, 3*sizeof(float));
    out.max_disp = r.get<float>();
    out.parallax_radius = r.get<float>();
    out.atlas_w = r.get<uint32_t>();
    out.atlas_h = r.get<uint32_t>();
    out.disp_bits = static_cast<int>(r.get<uint32_t>());
    const uint32_t vc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* vp = r.take(vc*sizeof(CageVert));
    if (!r.ok) return false;
    out.verts.resize(vc); std::memcpy(out.verts.data(), vp, vc*sizeof(CageVert));
    const uint32_t tc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* tp = r.take(tc*sizeof(CageTri));
    if (!r.ok) return false;
    out.tris.resize(tc); std::memcpy(out.tris.data(), tp, tc*sizeof(CageTri));
    const uint32_t dc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* dp = r.take(dc);
    if (!r.ok) return false;
    out.disp.assign(dp, dp+dc);
    const uint32_t cc = r.get<uint32_t>();
    if (!r.ok) return false;
    const uint8_t* cp = r.take(cc);
    if (!r.ok) return false;
    out.color.assign(cp, cp+cc);
    return true;
}

bool build_cage(const std::vector<Tri>&, const ImpGenParams&, uint64_t, ImposterAsset&) { return false; }
bool bake_displacement_cpu(const std::vector<Tri>&, ImposterAsset&) { return false; }

} // namespace imposter_asset
