#include "surface_proxy.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>

namespace surface_proxy {
namespace {
float component(mm::Vec3 v,int i) { return i==0?v.x:i==1?v.y:v.z; }
void set(mm::Vec3& v,int i,float x) { if(i==0)v.x=x; else if(i==1)v.y=x; else v.z=x; }
double dot3(mm::Vec3 a,mm::Vec3 b) { return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z; }
mm::Vec3 cross3(mm::Vec3 a,mm::Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
bool finite(mm::Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
mm::Vec3 normal(const sparse_voxel::Triangle& t) {
    const auto a=t.positions[0],b=t.positions[1],c=t.positions[2];
    return {(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
        (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
        (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
}
float magnitude(mm::Vec3 v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
mm::Vec3 unit(mm::Vec3 v) { const float d=magnitude(v); return d>1e-20f?mm::Vec3{v.x/d,v.y/d,v.z/d}:mm::Vec3{0,1,0}; }
uint32_t byte(float v) { return uint32_t(std::lround(std::clamp(v,0.0f,1.0f)*255)); }
uint32_t pack(float x,float y,float z,float w=1) { return byte(x)|(byte(y)<<8)|(byte(z)<<16)|(byte(w)<<24); }
float channel(uint32_t p,int i) { return float((p>>(8*i))&255)/255; }
Texel texel(mm::Vec3 c,mm::Vec3 n,float a) {
    n=unit(n); return {pack(c.x,c.y,c.z,a),pack(n.x*.5f+.5f,n.y*.5f+.5f,n.z*.5f+.5f)};
}
bool source_valid(const std::vector<sparse_voxel::Triangle>& source,std::string& error) {
    if(source.size()>2000000) { error="surface source exceeds triangle budget"; return false; }
    for(const auto& t:source) {
        if(!finite(t.surface.albedo) || !std::isfinite(t.surface.coverage) ||
            t.surface.coverage<0 || t.surface.coverage>1) { error="invalid source surface"; return false; }
        for(int i=0;i<3;++i) if(!finite(t.positions[i]) || !std::isfinite(t.uv[i].x) ||
            !std::isfinite(t.uv[i].y)) { error="invalid source vertex"; return false; }
        if(!finite(normal(t)) || !std::isfinite(magnitude(normal(t)))) { error="source normal overflow"; return false; }
    }
    return true;
}
using ComponentMap=std::map<size_t,std::vector<const sparse_voxel::Triangle*>>;
ComponentMap components(const std::vector<sparse_voxel::Triangle>& source) {
    // Shared edges define a component. A shared attachment point alone must
    // not merge distinct needles in a fascicle, nor attach them to a twig.
    std::vector<size_t> parents(source.size()); for(size_t i=0;i<parents.size();++i) parents[i]=i;
    const auto root=[&](size_t i) { while(i!=parents[i]) { parents[i]=parents[parents[i]]; i=parents[i]; } return i; };
    using Point=std::array<float,3>;
    std::map<std::pair<Point,Point>,size_t> edges;
    for(size_t i=0;i<source.size();++i) {
        const auto& t=source[i]; if(magnitude(normal(t))<=1e-20f || t.surface.coverage==0) continue;
        for(int k=0;k<3;++k) {
            const auto a=t.positions[k],b=t.positions[(k+1)%3];
            Point p{a.x,a.y,a.z},q{b.x,b.y,b.z}; if(q<p) std::swap(p,q);
            const auto inserted=edges.emplace(std::make_pair(p,q),i);
            if(!inserted.second) parents[root(i)]=root(inserted.first->second);
        }
    }
    ComponentMap groups;
    for(size_t i=0;i<source.size();++i) if(magnitude(normal(source[i]))>1e-20f && source[i].surface.coverage>0)
        groups[root(i)].push_back(&source[i]);
    return groups;
}
void principal_frame(const std::vector<const sparse_voxel::Triangle*>& triangles,mm::Vec3& u,mm::Vec3& v,mm::Vec3& n,float* thinness=nullptr) {
    double center[3]{},cov[3][3]{},vectors[3][3]={{1,0,0},{0,1,0},{0,0,1}};
    const double count=double(triangles.size())*3;
    for(const auto* t:triangles) for(auto p:t->positions) for(int i=0;i<3;++i) center[i]+=component(p,i)/count;
    for(const auto* t:triangles) for(auto p:t->positions) for(int i=0;i<3;++i) for(int j=0;j<3;++j)
        cov[i][j]+=(component(p,i)-center[i])*(component(p,j)-center[j])/count;
    for(int sweep=0;sweep<20;++sweep) {
        int a=0,b=1;
        for(int i=0;i<3;++i) for(int j=i+1;j<3;++j) if(std::abs(cov[i][j])>std::abs(cov[a][b])) { a=i;b=j; }
        if(std::abs(cov[a][b])<1e-30) break;
        const double angle=.5*std::atan2(2*cov[a][b],cov[b][b]-cov[a][a]),c=std::cos(angle),s=std::sin(angle);
        const double aa=c*c*cov[a][a]-2*s*c*cov[a][b]+s*s*cov[b][b];
        const double bb=s*s*cov[a][a]+2*s*c*cov[a][b]+c*c*cov[b][b];
        for(int i=0;i<3;++i) if(i!=a && i!=b) {
            const double ia=c*cov[i][a]-s*cov[i][b],ib=s*cov[i][a]+c*cov[i][b];
            cov[i][a]=cov[a][i]=ia;cov[i][b]=cov[b][i]=ib;
        }
        cov[a][a]=aa;cov[b][b]=bb;cov[a][b]=cov[b][a]=0;
        for(int i=0;i<3;++i) {
            const double ia=c*vectors[i][a]-s*vectors[i][b],ib=s*vectors[i][a]+c*vectors[i][b];
            vectors[i][a]=ia;vectors[i][b]=ib;
        }
    }
    std::array<int,3> order{0,1,2};std::sort(order.begin(),order.end(),[&](int a,int b){return cov[a][a]<cov[b][b];});
    const int least=order[0],most=order[2];
    if(thinness) *thinness=float(std::sqrt(std::max(0.0,cov[least][least])/std::max(1e-30,cov[order[1]][order[1]])));
    n=unit({float(vectors[0][least]),float(vectors[1][least]),float(vectors[2][least])});
    u=unit({float(vectors[0][most]),float(vectors[1][most]),float(vectors[2][most])});
    v=unit(cross3(n,u)); u=unit(cross3(v,n));
}
void mips(Texture& texture) {
    const auto& base=texture.mips.front();
    const double coverage=double(std::count_if(base.texels.begin(),base.texels.end(),
        [](Texel t){return channel(t.color,3)>=.5f;}))/base.texels.size();
    while(texture.mips.back().width>1 || texture.mips.back().height>1) {
        const auto& src=texture.mips.back();
        Mip dst; dst.width=std::max(1u,src.width/2); dst.height=std::max(1u,src.height/2);
        dst.texels.resize(size_t(dst.width)*dst.height);
        for(uint32_t y=0;y<dst.height;++y) for(uint32_t x=0;x<dst.width;++x) {
            double alpha=0,c[3]{},n[3]{};
            for(int j=0;j<2;++j) for(int i=0;i<2;++i) {
                const auto t=src.texels[size_t(std::min(src.height-1,y*2+j))*src.width+std::min(src.width-1,x*2+i)];
                const double a=channel(t.color,3); alpha+=a;
                for(int k=0;k<3;++k) { c[k]+=channel(t.color,k)*a; n[k]+=(channel(t.normal,k)*2-1)*a; }
            }
            if(alpha>0) dst.texels[size_t(y)*dst.width+x]=texel(
                {float(c[0]/alpha),float(c[1]/alpha),float(c[2]/alpha)},
                {float(n[0]),float(n[1]),float(n[2])},float(alpha*.25));
        }
        // Preserve cutout coverage at minification without altering the
        // premultiplied filtering data used to construct subsequent levels.
        std::vector<float> alphas; alphas.reserve(dst.texels.size());
        for(auto t:dst.texels) alphas.push_back(channel(t.color,3));
        std::sort(alphas.begin(),alphas.end(),std::greater<float>());
        const size_t wanted=size_t(std::lround(coverage*alphas.size()));
        if(wanted && alphas[wanted-1]>0) {
            const float threshold=wanted<alphas.size()?(alphas[wanted-1]+alphas[wanted])*.5f:alphas.back();
            dst.alpha_scale=std::clamp(.5f/std::max(threshold,1e-5f),.25f,8.0f);
        }
        texture.mips.push_back(std::move(dst));
    }
}
void compact_texture(Texture& texture) {
    constexpr uint32_t size=texture_page_size;
    for(auto& mip:texture.mips) {
        std::vector<Texel> packed;
        const uint32_t columns=(mip.width+size-1)/size,rows=(mip.height+size-1)/size;
        mip.tiles.resize(size_t(columns)*rows);
        for(uint32_t y=0;y<rows;++y) for(uint32_t x=0;x<columns;++x) {
            TexturePage page;page.first_texel=uint32_t(packed.size());
            for(uint32_t j=0;j<size;++j) for(uint32_t i=0;i<size;++i) if(x*size+i<mip.width && y*size+j<mip.height) {
                const auto texel=mip.texels[size_t(y*size+j)*mip.width+x*size+i];
                if(texel.color>>24) {
                    const uint32_t bit=j*size+i;
                    if(bit<32) page.mask_lo|=1u<<bit;else page.mask_hi|=1u<<(bit-32);
                    packed.push_back(texel);
                }
            }
            if(page.mask_lo || page.mask_hi) {mip.tiles[size_t(y)*columns+x]=uint32_t(mip.pages.size())+1;mip.pages.push_back(page);}
        }
        if(packed.size()*sizeof(Texel)+mip.pages.size()*sizeof(TexturePage)+mip.tiles.size()*sizeof(uint32_t)<mip.texels.size()*sizeof(Texel)) mip.texels=std::move(packed);
        else {mip.pages.clear();mip.tiles.clear();}
    }
}
uint32_t population_count(uint32_t bits) {
    uint32_t count=0;while(bits) {bits&=bits-1;++count;}return count;
}
struct RasterTriangle { sparse_voxel::Triangle triangle; mm::Vec3 normal; };
bool raster_texture(const std::vector<RasterTriangle>& source,mm::Vec3 basis_u,mm::Vec3 basis_v,mm::Vec3 basis_n,
    const double* mn,const double* mx,uint32_t width,uint32_t height,uint32_t ss,uint64_t& sample_tests,
    uint64_t sample_budget,Texture& texture,std::string& error) {
    struct Sample { float depth=-std::numeric_limits<float>::infinity(),alpha=0; mm::Vec3 color{},normal{}; };
    const uint32_t sample_width=width*ss,sample_height=height*ss;
    std::vector<Sample> samples(size_t(sample_width)*sample_height);
    for(const auto& input:source) {
        const auto* t=&input.triangle;
        double x[3],y[3];
        for(int i=0;i<3;++i) { x[i]=(dot3(t->positions[i],basis_u)-mn[0])/(mx[0]-mn[0])*sample_width;
            y[i]=(dot3(t->positions[i],basis_v)-mn[1])/(mx[1]-mn[1])*sample_height; }
        const double det=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);
        if(std::abs(det)<1e-20) continue;
        const int x0=std::max(0,int(std::floor(std::min({x[0],x[1],x[2]}))));
        const int y0=std::max(0,int(std::floor(std::min({y[0],y[1],y[2]}))));
        const int x1=std::min(int(sample_width)-1,int(std::ceil(std::max({x[0],x[1],x[2]}))));
        const int y1=std::min(int(sample_height)-1,int(std::ceil(std::max({y[0],y[1],y[2]}))));
        sample_tests+=uint64_t(x1-x0+1)*(y1-y0+1);
        if(sample_tests>sample_budget) { error="surface raster sample budget exceeded"; return false; }
        const auto n=input.normal;
        for(int py=y0;py<=y1;++py) for(int px=x0;px<=x1;++px) {
            const double a=((y[1]-y[2])*(px+.5-x[2])+(x[2]-x[1])*(py+.5-y[2]))/det;
            const double b=((y[2]-y[0])*(px+.5-x[2])+(x[0]-x[2])*(py+.5-y[2]))/det,c=1-a-b;
            if(a<0 || b<0 || c<0) continue;
            const float depth=float(a*dot3(t->positions[0],basis_n)+b*dot3(t->positions[1],basis_n)+c*dot3(t->positions[2],basis_n));
            auto& s=samples[size_t(py)*sample_width+px];
            if(depth>=s.depth) s={depth,t->surface.coverage,t->surface.albedo,n};
        }
    }
    Mip mip; mip.width=width; mip.height=height; mip.texels.resize(size_t(width)*height);
    for(uint32_t py=0;py<height;++py) for(uint32_t px=0;px<width;++px) {
        double a=0,c[3]{},n[3]{};
        for(uint32_t j=0;j<ss;++j) for(uint32_t i=0;i<ss;++i) {
            const auto& s=samples[size_t(py*ss+j)*sample_width+px*ss+i]; a+=s.alpha;
            for(int k=0;k<3;++k) { c[k]+=component(s.color,k)*s.alpha; n[k]+=component(s.normal,k)*s.alpha; }
        }
        if(a>0) mip.texels[size_t(py)*width+px]=texel({float(c[0]/a),float(c[1]/a),float(c[2]/a)},
            {float(n[0]),float(n[1]),float(n[2])},float(a/(ss*ss)));
    }
    texture.mips.push_back(std::move(mip)); mips(texture);
    return true;
}
} // namespace

bool validate(const Asset& asset,std::string& error) {
    error.clear();
    if(asset.triangles.size()>2000000 || asset.textures.size()>65536) { error="surface asset exceeds addressing budget"; return false; }
    uint64_t count=0;
    for(const auto& texture:asset.textures) {
        if(texture.mips.empty() || texture.mips.size()>13) { error="surface texture has no/too many mips"; return false; }
        uint32_t w=texture.mips[0].width,h=texture.mips[0].height;
        if(!w || !h || w>4096 || h>4096 || (w&(w-1)) || (h&(h-1))) { error="surface texture dimensions must be powers of two <=4096"; return false; }
        for(size_t i=0;i<texture.mips.size();++i) {
            const auto& m=texture.mips[i]; count+=m.texels.size();
            const uint64_t page_count=uint64_t((w+7)/8)*((h+7)/8);
            const bool valid_storage=m.tiles.empty()?m.pages.empty() && m.texels.size()==uint64_t(w)*h:
                m.tiles.size()==page_count && std::all_of(m.tiles.begin(),m.tiles.end(),[&](uint32_t p){return p<=m.pages.size();}) &&
                std::all_of(m.pages.begin(),m.pages.end(),[&](TexturePage p){return
                    uint64_t(p.first_texel)+population_count(p.mask_lo)+population_count(p.mask_hi)<=m.texels.size();});
            count+=(m.pages.size()*sizeof(TexturePage)+m.tiles.size()*sizeof(uint32_t)+sizeof(Texel)-1)/sizeof(Texel);
            if(m.width!=w || m.height!=h || !valid_storage ||
               !std::isfinite(m.alpha_scale) || m.alpha_scale<=0 || m.alpha_scale>8 ||
               (w==1&&h==1&&i+1!=texture.mips.size())) { error="invalid surface texture mip chain"; return false; }
            w=std::max(1u,w/2); h=std::max(1u,h/2);
        }
        if(texture.mips.back().width!=1 || texture.mips.back().height!=1 || count>64u*1024*1024) {
            error="surface texture mip chain incomplete or too large"; return false;
        }
    }
    for(const auto& t:asset.triangles) {
        if(t.projection_axis < -1 || t.projection_axis>2) { error="invalid surface projection axis"; return false; }
        if(t.texture!=no_texture && t.texture>=asset.textures.size()) { error="surface texture index out of range"; return false; }
        for(const auto& v:t.vertices) if(!finite(v.position)||!finite(v.normal)||!finite(v.albedo)||
            !std::isfinite(v.uv.x)||!std::isfinite(v.uv.y)||!std::isfinite(magnitude(v.normal))||magnitude(v.normal)<1e-10f) {
            error="invalid surface vertex"; return false;
        }
    }
    return true;
}

bool make_solid(const std::vector<sparse_voxel::Triangle>& source,Asset& out,std::string& error) {
    error.clear(); if(!source_valid(source,error)) return false;
    Asset candidate; candidate.triangles.reserve(source.size());
    for(const auto& t:source) {
        if(t.surface.coverage==0) continue;
        if(t.surface.coverage!=1) { error="solid surface cannot represent fractional coverage"; return false; }
        Triangle tri; const auto n=unit(normal(t));
        for(int i=0;i<3;++i) tri.vertices[i]={t.positions[i],n,t.surface.albedo,t.uv[i]};
        candidate.triangles.push_back(tri);
    }
    out=std::move(candidate); return true;
}

namespace {
bool config_valid(const Config& config,std::string& error) {
    if(config.resolution<4 || config.resolution>1024 || (config.resolution&(config.resolution-1)) ||
       !config.supersample || config.supersample>4 || !std::isfinite(config.max_plane_error) || config.max_plane_error<0 ||
       !config.max_patches || !config.max_texels || !config.max_sample_tests) {
        error="invalid surface patch bake configuration"; return false;
    }
    return true;
}
bool bake_group_cards(const ComponentMap& groups,const Config& config,bool compact,Asset& out,std::string& error) {
    Asset candidate; uint64_t texel_count=0,sample_tests=0;
    for(const auto& entry:groups) {
        mm::Vec3 basis_u,basis_v,basis_n;
        principal_frame(entry.second,basis_u,basis_v,basis_n);
        double mn[2]={1e30,1e30},mx[2]={-1e30,-1e30},near_depth=1e30,far_depth=-1e30;
        for(const auto* t:entry.second) for(auto p:t->positions) {
            mn[0]=std::min(mn[0],dot3(p,basis_u)); mn[1]=std::min(mn[1],dot3(p,basis_v));
            mx[0]=std::max(mx[0],dot3(p,basis_u)); mx[1]=std::max(mx[1],dot3(p,basis_v));
            near_depth=std::min(near_depth,dot3(p,basis_n));far_depth=std::max(far_depth,dot3(p,basis_n));
        }
        if((far_depth-near_depth)*.5>config.max_plane_error || entry.second.size()<=2) {
            for(const auto* t:entry.second) {
                if(t->surface.coverage!=1) { error="nonplanar fractional source needs explicit texture sampling"; return false; }
                Triangle solid; const auto n=unit(normal(*t));
                for(int i=0;i<3;++i) solid.vertices[i]={t->positions[i],n,t->surface.albedo,t->uv[i]};
                candidate.triangles.push_back(solid);
            }
            continue;
        }
        if(mx[0]<=mn[0] || mx[1]<=mn[1]) continue;
        if(candidate.textures.size()>=config.max_patches) { error="surface patch count budget exceeded"; return false; }
        const double plane_depth=(near_depth+far_depth)*.5;
        const double largest=std::max(mx[0]-mn[0],mx[1]-mn[1]);
        uint32_t dimensions[2]={config.resolution,config.resolution};
        for(int k=0;k<2;++k) while(dimensions[k]>4 && double(dimensions[k])/2>=config.resolution*(mx[k]-mn[k])/largest) dimensions[k]/=2;
        for(int k=0;k<2;++k) { const double pad=(mx[k]-mn[k])/(dimensions[k]-2); mn[k]-=pad; mx[k]+=pad; }
        const uint32_t width=dimensions[0],height=dimensions[1],ss=config.supersample;
        uint64_t mip_texels=0;
        for(uint32_t w=width,h=height;;w=std::max(1u,w/2),h=std::max(1u,h/2)) { mip_texels+=uint64_t(w)*h;if(w==1&&h==1)break; }
        if(texel_count+mip_texels>config.max_texels) { error="surface texture budget exceeded"; return false; }
        texel_count+=mip_texels;
        std::vector<RasterTriangle> raster; raster.reserve(entry.second.size());
        for(const auto* t:entry.second) raster.push_back({*t,unit(normal(*t))});
        Texture texture;
        if(!raster_texture(raster,basis_u,basis_v,basis_n,mn,mx,width,height,ss,sample_tests,
                           config.max_sample_tests,texture,error)) return false;
        if(compact) compact_texture(texture);
        const uint32_t texture_index=uint32_t(candidate.textures.size()); candidate.textures.push_back(std::move(texture));
        Vertex corners[4]; const int ux[]={0,1,1,0},vy[]={0,0,1,1};
        for(int i=0;i<4;++i) {
            auto& vert=corners[i];
            const float pu=float(ux[i]?mx[0]:mn[0]),pv=float(vy[i]?mx[1]:mn[1]),pn=float(plane_depth);
            vert.position={basis_u.x*pu+basis_v.x*pv+basis_n.x*pn,
                basis_u.y*pu+basis_v.y*pv+basis_n.y*pn,basis_u.z*pu+basis_v.z*pv+basis_n.z*pn};
            vert.normal=basis_n; vert.albedo={1,1,1}; vert.uv={float(ux[i]),float(vy[i])};
        }
        candidate.triangles.push_back({{corners[0],corners[1],corners[2]},texture_index});
        candidate.triangles.push_back({{corners[0],corners[2],corners[3]},texture_index});
    }
    if(!validate(candidate,error)) return false;
    out=std::move(candidate); return true;
}


} // namespace

bool bake_cards(const std::vector<sparse_voxel::Triangle>& source,const Config& config,Asset& out,std::string& error) {
    error.clear();
    if(!source_valid(source,error) || !config_valid(config,error)) return false;
    return bake_group_cards(components(source),config,false,out,error);
}

bool bake_clusters(const std::vector<sparse_voxel::Triangle>& source,const ClusterConfig& config,
                   Asset& out,ClusterStats& stats,std::string& error) {
    error.clear();
    if(!source_valid(source,error) || !config_valid(config.patch,error)) return false;
    if(!std::isfinite(config.thin_ratio) || config.thin_ratio<=0 || config.thin_ratio>=1 ||
       !std::isfinite(config.min_normal_alignment) || config.min_normal_alignment<=0 || config.min_normal_alignment>1 ||
       !std::isfinite(config.max_patch_diameter) || config.max_patch_diameter<=0 || !config.max_fit_tests) {
        error="invalid surface cluster configuration"; return false;
    }
    struct Piece {
        std::vector<const sparse_voxel::Triangle*> triangles;
        mm::Vec3 center{},normal{};
        double area=0;
        bool used=false;
    };
    std::vector<Piece> pieces;
    ComponentMap grouped;
    ClusterStats measured;
    const double diameter2=double(config.max_patch_diameter)*config.max_patch_diameter;
    const auto distance2=[](mm::Vec3 a,mm::Vec3 b) {
        const mm::Vec3 d{a.x-b.x,a.y-b.y,a.z-b.z};return dot3(d,d);
    };
    const auto bound=[](const std::vector<const sparse_voxel::Triangle*>& ts,mm::Vec3 n) {
        double low=1e30,high=-1e30;
        for(const auto* t:ts) for(auto p:t->positions) {const auto d=dot3(p,n);low=std::min(low,d);high=std::max(high,d);}
        return (high-low)*.5;
    };
    // Full components preserve their curved needle support. Solid twigs must
    // not become eligible merely because their absolute radius is small.
    for(auto& entry:components(source)) {
        ++measured.source_components;
        mm::Vec3 u,v,n;float thinness=0;
        principal_frame(entry.second,u,v,n,&thinness);
        if(thinness>config.thin_ratio || bound(entry.second,n)>config.patch.max_plane_error) {
            // Separate single triangles force the common patch emitter to
            // retain this component exactly, regardless of the absolute bound.
            for(const auto* t:entry.second) grouped[grouped.size()]={t};
            measured.retained_triangles+=uint32_t(entry.second.size());
            continue;
        }
        Piece piece;piece.triangles=std::move(entry.second);piece.normal=n;
        for(const auto* t:piece.triangles) {
            piece.area+=magnitude(normal(*t));
            for(auto p:t->positions) for(int k=0;k<3;++k)
                set(piece.center,k,component(piece.center,k)+float(component(p,k)/(3.0*piece.triangles.size())));
        }
        pieces.push_back(std::move(piece));++measured.thin_components;
    }
    // Largest support seeds first; stable ordering makes repeated bakes agree.
    std::stable_sort(pieces.begin(),pieces.end(),[](const Piece& a,const Piece& b){return a.area>b.area;});
    for(size_t seed=0;seed<pieces.size();++seed) {
        if(pieces[seed].used) continue;
        auto combined=pieces[seed].triangles;pieces[seed].used=true;
        std::vector<size_t> members{seed};
        mm::Vec3 u,v,n;principal_frame(combined,u,v,n);
        for(;;) {
            size_t best=pieces.size();double best_score=std::numeric_limits<double>::infinity();
            for(size_t i=0;i<pieces.size();++i) {
                if(pieces[i].used || std::abs(dot3(n,pieces[i].normal))<config.min_normal_alignment) continue;
                double nearest=std::numeric_limits<double>::infinity();bool too_far=false;
                for(size_t m:members) {const double d=distance2(pieces[m].center,pieces[i].center);nearest=std::min(nearest,d);too_far|=d>diameter2;}
                if(too_far) continue;
                if(++measured.fit_tests>config.max_fit_tests) {error="surface cluster fit budget exceeded";return false;}
                auto trial=combined;trial.insert(trial.end(),pieces[i].triangles.begin(),pieces[i].triangles.end());
                mm::Vec3 lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
                for(const auto* t:trial) for(auto p:t->positions) for(int k=0;k<3;++k) {
                    set(lo,k,std::min(component(lo,k),component(p,k)));
                    set(hi,k,std::max(component(hi,k),component(p,k)));
                }
                if(distance2(lo,hi)>diameter2) continue;
                mm::Vec3 tu,tv,tn;principal_frame(trial,tu,tv,tn);
                const double deviation=bound(trial,tn);
                if(deviation>config.patch.max_plane_error || std::abs(dot3(tn,pieces[i].normal))<config.min_normal_alignment) continue;
                bool aligned=true;for(size_t m:members) aligned&=std::abs(dot3(tn,pieces[m].normal))>=config.min_normal_alignment;
                if(!aligned) continue;
                const double score=nearest+deviation*deviation;
                if(score<best_score) {best=i;best_score=score;}
            }
            if(best==pieces.size()) break;
            combined.insert(combined.end(),pieces[best].triangles.begin(),pieces[best].triangles.end());
            members.push_back(best);pieces[best].used=true;principal_frame(combined,u,v,n);
        }
        measured.max_projection_error=std::max(measured.max_projection_error,float(bound(combined,n)));
        ++measured.merged_patches;
        grouped[grouped.size()]=std::move(combined);
    }
    Asset candidate;
    if(!bake_group_cards(grouped,config.patch,true,candidate,error)) return false;
    out=std::move(candidate);stats=measured;return true;
}

bool bake_cluster_lods(const std::vector<sparse_voxel::Triangle>& source,const ClusterLodConfig& config,
                      std::vector<ClusterLod>& out,std::string& error) {
    error.clear();
    if(config.levels.empty() || config.levels.size()>16 || !config.max_resident_bytes) {
        error="cluster LOD bake requires 1-16 levels and a resident budget";return false;
    }
    float previous=-1;
    for(const auto& c:config.levels) {
        if(!std::isfinite(c.patch.max_plane_error) || c.patch.max_plane_error<=previous) {
            error="cluster LOD projection targets must increase";return false;
        }
        previous=c.patch.max_plane_error;
    }
    std::vector<ClusterLod> result;result.reserve(config.levels.size());uint64_t bytes=0;
    for(const auto& c:config.levels) {
        ClusterLod level;level.requested_error=c.patch.max_plane_error;
        if(!bake_clusters(source,c,level.asset,level.stats,error)) return false;
        if(!result.empty() && level.asset.triangles.size()>=result.back().asset.triangles.size()) continue;
        uint64_t used=level.asset.triangles.size()*sizeof(Triangle);
        for(const auto& texture:level.asset.textures) for(const auto& mip:texture.mips)
            used+=mip.texels.size()*sizeof(Texel)+mip.pages.size()*sizeof(TexturePage)+mip.tiles.size()*sizeof(uint32_t);
        if(used>config.max_resident_bytes-bytes) {error="cluster LOD resident budget exceeded";return false;}
        bytes+=used;result.push_back(std::move(level));
    }
    out=std::move(result);return true;
}

bool cluster_lod_switch_distances(const std::vector<ClusterLod>& levels,const LodProjection& projection,
                                 std::vector<float>& out,std::string& error) {
    error.clear();
    if(levels.empty() || levels.size()>16 || !projection.width || !projection.height ||
       !std::isfinite(projection.vertical_fov_radians) || projection.vertical_fov_radians<=0 ||
       projection.vertical_fov_radians>=3.14159265358979323846 ||
       !std::isfinite(projection.pixel_error) || projection.pixel_error<=0) {
        error="invalid cluster LOD projection";return false;
    }
    double lo[3]={1e30,1e30,1e30},hi[3]={-1e30,-1e30,-1e30};
    for(const auto& t:levels.front().asset.triangles) for(const auto& v:t.vertices) for(int k=0;k<3;++k) {
        const double p=component(v.position,k);lo[k]=std::min(lo[k],p);hi[k]=std::max(hi[k],p);
    }
    double radius=0;for(int k=0;k<3;++k) radius+=(hi[k]-lo[k])*(hi[k]-lo[k])*.25;
    radius=std::sqrt(radius);
    if(!(radius>0) || !std::isfinite(radius) || levels.front().asset.triangles.empty()) {
        error="cluster LOD has empty or invalid finest bounds";return false;
    }
    float previous=-1;
    for(const auto& l:levels) {
        if(!std::isfinite(l.requested_error) || l.requested_error<=previous ||
           !std::isfinite(l.stats.max_projection_error) || l.stats.max_projection_error<0 ||
           l.stats.max_projection_error>l.requested_error || !validate(l.asset,error)) {
            error="invalid cluster LOD error metadata or asset";return false;
        }
        previous=l.requested_error;
    }
    const double tangent=std::tan(projection.vertical_fov_radians*.5);
    const double focal=projection.height/(2*tangent),aspect=double(projection.width)/projection.height;
    const double angular=std::sqrt(1+tangent*tangent*(1+aspect*aspect));
    // The original source lies within the finest patches plus their measured
    // displacement. Use this conservative source sphere for the near plane.
    const double source_radius=radius+levels.front().stats.max_projection_error;
    double last=0;std::vector<float> distances;distances.reserve(levels.size());
    for(size_t i=1;i<levels.size();++i) {
        const double e=levels[i].stats.max_projection_error;
        const double distance=angular*(source_radius+e)+angular*angular*e*focal/projection.pixel_error;
        last=std::max(last,distance/radius);
        if(!std::isfinite(last) || last>std::numeric_limits<float>::max()) {error="cluster LOD switch distance overflow";return false;}
        // Round outward: a float conversion must not enter a coarse level early.
        distances.push_back(std::nextafter(float(last),std::numeric_limits<float>::infinity()));
    }
    distances.push_back(std::numeric_limits<float>::infinity());out=std::move(distances);return true;
}

bool bake_layers(const std::vector<sparse_voxel::Triangle>& source,const LayerConfig& config,Asset& out,std::string& error) {
    error.clear(); if(!source_valid(source,error)) return false;
    if(!std::isfinite(config.spacing) || config.spacing<=0 || !std::isfinite(config.thin_ratio) ||
       config.thin_ratio<=0 || config.thin_ratio>1 || config.resolution<4 || config.resolution>1024 ||
       (config.resolution&(config.resolution-1)) || !config.supersample || config.supersample>4 ||
       !config.max_layers_per_axis || config.max_layers_per_axis>512 || !config.max_texels || !config.max_sample_tests) {
        error="invalid layered surface bake configuration"; return false;
    }
    Asset candidate;
    std::vector<const sparse_voxel::Triangle*> thin;
    for(const auto& group:components(source)) {
        mm::Vec3 u,v,n;float thinness=0;principal_frame(group.second,u,v,n,&thinness);
        if(thinness<=config.thin_ratio) {
            thin.insert(thin.end(),group.second.begin(),group.second.end());
        } else for(const auto* t:group.second) {
            if(t->surface.coverage!=1) { error="round fractional component requires explicit texture sampling"; return false; }
            Triangle solid; const auto normal_value=unit(normal(*t));
            for(int k=0;k<3;++k) solid.vertices[k]={t->positions[k],normal_value,t->surface.albedo,t->uv[k]};
            candidate.triangles.push_back(solid);
        }
    }
    if(thin.empty()) { out=std::move(candidate); return true; }
    double minimum[3]={1e30,1e30,1e30},maximum[3]={-1e30,-1e30,-1e30};
    for(const auto* t:thin) for(auto p:t->positions) for(int k=0;k<3;++k) {
        minimum[k]=std::min(minimum[k],double(component(p,k))); maximum[k]=std::max(maximum[k],double(component(p,k)));
    }
    const double largest=std::max({maximum[0]-minimum[0],maximum[1]-minimum[1],maximum[2]-minimum[2]});
    uint64_t texel_count=0,sample_tests=0;
    const auto clip=[](const std::vector<mm::Vec3>& input,int axis,double plane,bool upper) {
        std::vector<mm::Vec3> output;
        if(input.empty()) return output;
        for(size_t i=0;i<input.size();++i) {
            const auto a=input[i],b=input[(i+1)%input.size()];
            const double da=component(a,axis)-plane,db=component(b,axis)-plane;
            const bool ia=upper?da<=0:da>=0,ib=upper?db<=0:db>=0;
            if(ia) output.push_back(a);
            if(ia!=ib) {
                const double f=da/(da-db);
                output.push_back({float(a.x+(b.x-a.x)*f),float(a.y+(b.y-a.y)*f),float(a.z+(b.z-a.z)*f)});
                set(output.back(),axis,float(plane));
            }
        }
        return output;
    };
    for(int axis=0;axis<3;++axis) {
        const double span=maximum[axis]-minimum[axis];
        const double desired=std::max(1.0,std::ceil(span/config.spacing));
        if(desired>config.max_layers_per_axis) { error="layered surface exceeds depth layer budget"; return false; }
        const uint32_t count=uint32_t(desired);
        const double spacing=span>0?span/count:config.spacing;
        std::vector<std::vector<RasterTriangle>> layers(count);
        mm::Vec3 basis_u{},basis_v{},basis_n{};
        set(basis_u,(axis+1)%3,1); set(basis_v,(axis+2)%3,1); set(basis_n,axis,1);
        for(const auto* t:thin) {
            double lo=1e30,hi=-1e30;
            for(auto p:t->positions) { lo=std::min(lo,double(component(p,axis)));hi=std::max(hi,double(component(p,axis))); }
            const int first=std::clamp(int(std::floor((lo-minimum[axis])/spacing)),0,int(count)-1);
            const int last=std::clamp(int(std::floor((hi-minimum[axis])/spacing)),0,int(count)-1);
            const auto source_normal=unit(normal(*t));
            for(int layer=first;layer<=last;++layer) {
                const double lower=minimum[axis]+layer*spacing,upper=minimum[axis]+(layer+1)*spacing;
                const auto polygon=clip(clip({t->positions.begin(),t->positions.end()},axis,lower,false),axis,upper,true);
                for(size_t i=1;i+1<polygon.size();++i) {
                    RasterTriangle triangle{*t,source_normal};
                    triangle.triangle.positions={polygon[0],polygon[i],polygon[i+1]};
                    layers[layer].push_back(triangle);
                }
            }
        }
        for(uint32_t layer=0;layer<count;++layer) {
            const auto& triangles=layers[layer]; if(triangles.empty()) continue;
            double mn[2]={1e30,1e30},mx[2]={-1e30,-1e30};
            for(const auto& t:triangles) for(auto p:t.triangle.positions) {
                const double d[]={dot3(p,basis_u),dot3(p,basis_v)};
                for(int k=0;k<2;++k) { mn[k]=std::min(mn[k],d[k]);mx[k]=std::max(mx[k],d[k]); }
            }
            if(mx[0]<=mn[0] || mx[1]<=mn[1]) continue;
            uint32_t dimensions[2]={4,4};
            for(int k=0;k<2;++k) while(dimensions[k]<config.resolution && dimensions[k]<config.resolution*(mx[k]-mn[k])/largest) dimensions[k]*=2;
            for(int k=0;k<2;++k) { const double pad=(mx[k]-mn[k])/(dimensions[k]-2);mn[k]-=pad;mx[k]+=pad; }
            Texture texture;
            if(!raster_texture(triangles,basis_u,basis_v,basis_n,mn,mx,dimensions[0],dimensions[1],
                config.supersample,sample_tests,config.max_sample_tests,texture,error)) return false;
            if(std::none_of(texture.mips[0].texels.begin(),texture.mips[0].texels.end(),[](Texel t){return t.color>>24;})) continue;
            compact_texture(texture);
            for(const auto& mip:texture.mips) texel_count+=mip.texels.size()+(mip.pages.size()*sizeof(TexturePage)+mip.tiles.size()*sizeof(uint32_t)+sizeof(Texel)-1)/sizeof(Texel);
            if(texel_count>config.max_texels) { error="layered surface exceeds resident texture budget";return false; }
            const uint32_t texture_index=uint32_t(candidate.textures.size());candidate.textures.push_back(std::move(texture));
            Vertex corners[4]; const int ux[]={0,1,1,0},vy[]={0,0,1,1};
            for(int i=0;i<4;++i) {
                auto& vert=corners[i];vert.position={};
                set(vert.position,axis,float(minimum[axis]+(layer+.5)*spacing));
                set(vert.position,(axis+1)%3,float(ux[i]?mx[0]:mn[0]));
                set(vert.position,(axis+2)%3,float(vy[i]?mx[1]:mn[1]));
                vert.normal=basis_n;vert.albedo={1,1,1};vert.uv={float(ux[i]),float(vy[i])};
            }
            candidate.triangles.push_back({{corners[0],corners[1],corners[2]},texture_index,axis});
            candidate.triangles.push_back({{corners[0],corners[2],corners[3]},texture_index,axis});
        }
    }
    if(!validate(candidate,error)) return false;
    out=std::move(candidate);return true;
}
} // namespace surface_proxy
