#include "brick_bond_atlas.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace castle_bake {
namespace {
using gpu_meshing::FacePatch;
using matter::Float3;
Float3 add(Float3 a, Float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Float3 mul(Float3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
float dot(Float3 a, Float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Float3 cross(Float3 a, Float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
bool finite(Float3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
Float3 normal(Float3 a) { const float q=dot(a,a); return q>1e-20f?mul(a,1/std::sqrt(q)):Float3{0,0,1}; }
uint8_t byte(float x) { return static_cast<uint8_t>(std::lround(std::clamp(x,0.f,255.f))); }
uint32_t wrap(int x,uint32_t n) { int r=x%static_cast<int>(n); return static_cast<uint32_t>(r<0?r+n:r); }
struct Hash {
    uint64_t value=14695981039346656037ull;
    void u32(uint32_t v) { for(int i=0;i<4;++i) {value^=(v>>(i*8))&255u;value*=1099511628211ull;} }
    void u64(uint64_t v) {u32(static_cast<uint32_t>(v));u32(static_cast<uint32_t>(v>>32));}
    void f(float v) {uint32_t bits;std::memcpy(&bits,&v,4);u32(bits);}
    void v(Float3 x) {f(x.x);f(x.y);f(x.z);}
};
uint32_t selection(uint32_t col,uint32_t row,uint32_t columns,uint32_t seed) {
    uint32_t x=seed^0x9e3779b9u;
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
    // An odd affine permutation uses all16 faces in each16-cell block;
    // the default32-brick period therefore uses every source exactly twice.
    return (((row*columns+col)*((x>>4)|1u))+x)&15u;
}
bool valid_patch(const FacePatch& p,std::string& error) {
    const auto& l=p.layout;
    if(!l.width||!l.height||l.width>4096||l.height>4096||
       p.texels.size()!=size_t(l.width)*l.height||
       !std::isfinite(p.u_min_m)||!std::isfinite(p.u_max_m)||
       !std::isfinite(p.v_min_m)||!std::isfinite(p.v_max_m)||
       !(p.u_max_m>p.u_min_m)||!(p.v_max_m>p.v_min_m)||
       !std::isfinite(p.height_min_m)||!std::isfinite(p.height_max_m)||
       !(p.height_max_m>p.height_min_m)) {
        error="invalid finite face dimensions/storage/height interval";return false;
    }
    const auto& f=p.frame;
    if(!finite(f.origin_m)||!finite(f.u)||!finite(f.v)||!finite(f.n)||
       std::fabs(dot(f.u,f.u)-1)>1e-4f||std::fabs(dot(f.v,f.v)-1)>1e-4f||
       std::fabs(dot(f.n,f.n)-1)>1e-4f||std::fabs(dot(f.u,f.v))>1e-4f||
       dot(cross(f.u,f.v),f.n)<.9999f) {
        error="finite face frame must be proper orthonormal U/V/N";return false;
    }
    const float pu=(p.u_max_m-p.u_min_m)/l.width,pv=(p.v_max_m-p.v_min_m)/l.height;
    if(!std::isfinite(l.pitch_u_m)||!std::isfinite(l.pitch_v_m)||
       std::fabs(l.pitch_u_m-pu)>pu*1e-4f||std::fabs(l.pitch_v_m-pv)>pv*1e-4f) {
        error="finite face pitch does not match physical bounds";return false;
    }
    for(const auto& t:p.texels) {
        if(t.coverage>1u||!std::isfinite(t.height_m)||!finite(t.normal_uvn)||
           (t.coverage&&(std::fabs(dot(t.normal_uvn,t.normal_uvn)-1)>1e-3f||
                         t.normal_uvn.z<0||t.height_m<p.height_min_m||t.height_m>p.height_max_m))) {
            error="invalid finite face texel coverage/height/outward normal";return false;
        }
    }
    return true;
}
struct Sample {float coverage=0,height=0;Float3 n{0,0,0};};
Sample sample(const FacePatch& p,float u,float v) {
    Sample out;
    const float fx=(u-p.u_min_m)/p.layout.pitch_u_m-.5f;
    const float fy=(v-p.v_min_m)/p.layout.pitch_v_m-.5f;
    const int ix=static_cast<int>(std::floor(fx)),iy=static_cast<int>(std::floor(fy));
    const float tx=fx-ix,ty=fy-iy;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
        const int px=ix+x,py=iy+y;
        if(px<0||py<0||px>=static_cast<int>(p.layout.width)||py>=static_cast<int>(p.layout.height))continue;
        const auto& t=p.texels[size_t(py)*p.layout.width+px];
        const float w=(x?tx:1-tx)*(y?ty:1-ty)*t.coverage;
        out.coverage+=w;out.height+=w*t.height_m;out.n=add(out.n,mul(t.normal_uvn,w));
    }
    if(out.coverage>0) {out.height/=out.coverage;out.n=normal(out.n);}
    return out;
}
void hash_patch(Hash& h,const FacePatch& p) {
    h.u64(p.recipe_digest);h.u32(p.material);
    h.v(p.frame.origin_m);h.v(p.frame.u);h.v(p.frame.v);h.v(p.frame.n);
    h.v(p.layout.source_bounds.min_m);h.v(p.layout.source_bounds.max_m);
    h.u32(p.layout.width);h.u32(p.layout.height);h.f(p.layout.pitch_u_m);h.f(p.layout.pitch_v_m);
    h.f(p.u_min_m);h.f(p.u_max_m);h.f(p.v_min_m);h.f(p.v_max_m);
    h.f(p.height_min_m);h.f(p.height_max_m);
    for(const auto&t:p.texels){h.u32(t.coverage);h.f(t.height_m);h.v(t.normal_uvn);}
}
} // namespace

uint64_t brick_bond_recipe_digest(const BrickBondRecipe& r) {
    Hash hash;hash.u32(brick_bond_atlas_version);
    hash.f(r.brick_width_m);hash.f(r.brick_height_m);hash.f(r.pitch_u_m);hash.f(r.pitch_v_m);
    hash.u32(r.columns);hash.u32(r.rows);hash.u32(r.tile_pixels);hash.u32(r.seed);
    hash.f(r.nominal_face_height_m);hash.f(r.mortar_height_m);
    for(const auto& rgb:r.brick_rgb)for(auto c:rgb)hash.u32(c);
    for(auto c:r.mortar_rgb)hash.u32(c);
    hash.u32(r.brick_roughness);hash.u32(r.mortar_roughness);
    return hash.value;
}

bool build_brick_bond_atlas(const std::array<FacePatch,16>& patches,
                            const BrickBondRecipe& r,BrickBondAtlas& output,std::string& error) {
    error.clear();
    const float period=r.pitch_u_m*r.columns;
    if(!std::isfinite(r.brick_width_m)||!std::isfinite(r.brick_height_m)||
       !std::isfinite(r.pitch_u_m)||!std::isfinite(r.pitch_v_m)||
       !std::isfinite(r.nominal_face_height_m)||!std::isfinite(r.mortar_height_m)||
       !(r.brick_width_m>0&&r.brick_width_m<r.pitch_u_m)||
       !(r.brick_height_m>0&&r.brick_height_m<r.pitch_v_m)||
       !r.columns||r.columns>64||!r.rows||r.rows>128||(r.rows&1)||
       r.tile_pixels<16||r.tile_pixels>1024||
       !std::isfinite(period)||period<.001f||period>100.f||
       std::fabs(period-r.pitch_v_m*r.rows)>1e-6f) {
        error="bond requires positive physical brick/mortar gaps, even rows, square period and16..1024pixels";return false;
    }
    for(const auto& p:patches)if(!valid_patch(p,error))return false;
    Hash hash;hash.u64(brick_bond_recipe_digest(r));
    float height_min=r.mortar_height_m,height_max=r.mortar_height_m;
    for(const auto& p:patches) {
        hash_patch(hash,p);
        for(const auto& t:p.texels)if(t.coverage){
            height_min=std::min(height_min,t.height_m-r.nominal_face_height_m);
            height_max=std::max(height_max,t.height_m-r.nominal_face_height_m);
        }
    }
    if(!std::isfinite(height_min)||!std::isfinite(height_max)) {error="nonfinite composed metric height range";return false;}
    if(!(height_max>height_min))height_max=height_min+.000001f;
    BrickBondAtlas out;out.tile_pixels=r.tile_pixels;out.width=out.height=r.tile_pixels*4;
    out.actual_texels_per_m=r.tile_pixels/period;
    out.header.tile_size_m=period;
    // Existing gtex field is integer LOD metadata; physical sampling above is exact.
    out.header.texels_per_meter=static_cast<int32_t>(std::lround(out.actual_texels_per_m));
    out.content_digest=tileset::gtex_content_hash(hash.value,brick_bond_atlas_version);
    out.header.content_hash=out.content_digest;
    out.header.height_min=height_min;out.header.height_max=height_max;
    const size_t count=size_t(out.width)*out.height;
    out.albedo_rgb8.resize(count*3);out.normal_rg8.resize(count*2);
    out.orm_rgb8.resize(count*3);out.height_r16.resize(count);
    for(uint32_t y=0;y<r.tile_pixels;++y)for(uint32_t x=0;x<r.tile_pixels;++x) {
        const float u=(x+.5f)*period/r.tile_pixels,v=(y+.5f)*period/r.tile_pixels;
        const int row=static_cast<int>(std::floor(v/r.pitch_v_m));
        const float shift=(row&1)?r.pitch_u_m*.5f:0;
        const int col=static_cast<int>(std::floor((u-shift)/r.pitch_u_m));
        const float bu=u-shift-(col+.5f)*r.pitch_u_m,bv=v-(row+.5f)*r.pitch_v_m;
        const uint32_t source=selection(wrap(col,r.columns),wrap(row,r.rows),r.columns,r.seed);
        Sample s;
        if(std::fabs(bu)<=r.brick_width_m*.5f&&std::fabs(bv)<=r.brick_height_m*.5f)
            s=sample(patches[source],bu,bv);
        const float a=std::clamp(s.coverage,0.f,1.f);
        const Float3 n=normal(add(mul(s.n,a),{0,0,1-a}));
        const float h=(1-a)*r.mortar_height_m+a*(s.height-r.nominal_face_height_m);
        const uint16_t encoded_h=static_cast<uint16_t>(std::lround(std::clamp((h-height_min)/(height_max-height_min),0.f,1.f)*65535.f));
        // +U/+V normal components in the existing .gtex unit-normal RG convention.
        // The runtime's existing triplanar perturbation approximation is unchanged.
        for(uint32_t ty=0;ty<4;++ty)for(uint32_t tx=0;tx<4;++tx) {
            const size_t i=size_t(ty*r.tile_pixels+y)*out.width+tx*r.tile_pixels+x;
            for(int c=0;c<3;++c)out.albedo_rgb8[i*3+c]=byte((1-a)*r.mortar_rgb[c]+a*r.brick_rgb[source/2][c]);
            out.normal_rg8[i*2]=byte((n.x*.5f+.5f)*255);
            out.normal_rg8[i*2+1]=byte((n.y*.5f+.5f)*255);
            out.orm_rgb8[i*3]=255;
            out.orm_rgb8[i*3+1]=byte((1-a)*r.mortar_roughness+a*r.brick_roughness);
            out.orm_rgb8[i*3+2]=0;
            out.height_r16[i]=encoded_h;
        }
    }
    output=std::move(out);return true;
}
bool save_brick_bond_atlas(const std::string& path,const BrickBondAtlas& a,std::string& error) {
    const size_t count=size_t(a.width)*a.height;
    if(!a.tile_pixels||a.width!=4*a.tile_pixels||a.height!=a.width||
       a.albedo_rgb8.size()!=count*3||a.normal_rg8.size()!=count*2||
       a.orm_rgb8.size()!=count*3||a.height_r16.size()!=count) {
        error="incomplete brick bond atlas storage";return false;
    }
    return tileset::save_gtex(path,a.header,static_cast<int>(a.width),static_cast<int>(a.height),
                              a.albedo_rgb8.data(),a.normal_rg8.data(),a.orm_rgb8.data(),
                              a.height_r16.data(),error);
}
} // namespace castle_bake
