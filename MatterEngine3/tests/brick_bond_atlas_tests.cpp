#include "brick_bond_atlas.h"
#include "check.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <set>

namespace {
std::array<gpu_meshing::FacePatch,16> sources() {
    std::array<gpu_meshing::FacePatch,16> out;
    for(size_t i=0;i<out.size();++i) {
        auto& p=out[i];p.recipe_digest=100+i;p.material=7;
        p.frame.origin_m={0,.07f,0};
        if(i&1){p.frame.u={-1,0,0};p.frame.n={0,0,-1};}
        p.u_min_m=-.16f;p.u_max_m=.16f;p.v_min_m=-.08f;p.v_max_m=.08f;
        p.height_min_m=-.12f;p.height_max_m=.12f;
        p.layout.width=64;p.layout.height=32;
        p.layout.pitch_u_m=p.layout.pitch_v_m=.005f;
        p.layout.source_bounds={{-.15f,0,-.1f},{.15f,.14f,.1f}};
        p.texels.resize(64*32);
        const float nx=.20f,ny=-.10f,nz=std::sqrt(1-nx*nx-ny*ny);
        for(auto& t:p.texels){t.coverage=1;t.height_m=.10f-.0003f*float(i);t.normal_uvn={nx,ny,nz};}
    }
    return out;
}
void equal_pixel(const castle_bake::BrickBondAtlas& a,size_t p,size_t q) {
    for(int c=0;c<3;++c){CHECK(a.albedo_rgb8[3*p+c]==a.albedo_rgb8[3*q+c],"periodic albedo");CHECK(a.orm_rgb8[3*p+c]==a.orm_rgb8[3*q+c],"periodic ORM");}
    for(int c=0;c<2;++c)CHECK(a.normal_rg8[2*p+c]==a.normal_rg8[2*q+c],"periodic normal");
    CHECK(a.height_r16[p]==a.height_r16[q],"periodic height");
}
}
int main() {
    std::printf("brick bond atlas: physical running bond, coverage, normals,16Wang copies\n");
    auto patches=sources();castle_bake::BrickBondRecipe recipe;
    castle_bake::BrickBondAtlas atlas,again;std::string error;
    CHECK(castle_bake::build_brick_bond_atlas(patches,recipe,atlas,error),error.c_str());
    CHECK(atlas.width==2048&&atlas.height==2048,"default4x4 atlas dimensions");
    CHECK(atlas.header.tile_size_m==1.25f&&atlas.header.texels_per_meter==410,"physical period with rounded density metadata");
    CHECK(std::fabs(atlas.actual_texels_per_m-409.6f)<.001f,"actual density retained");
    CHECK(castle_bake::build_brick_bond_atlas(patches,recipe,again,error),error.c_str());
    CHECK(atlas.content_digest==again.content_digest&&atlas.albedo_rgb8==again.albedo_rgb8&&atlas.normal_rg8==again.normal_rg8&&atlas.orm_rgb8==again.orm_rgb8&&atlas.height_r16==again.height_r16,"complete deterministic atlas bytes and identity");
    const uint32_t t=atlas.tile_pixels,w=atlas.width;
    // Constant per-source faces isolate the periodic bond selection. Odd rows
    // cross the tile boundary through one brick; even rows cross mortar.
    for(uint32_t k=0;k<t;++k){equal_pixel(atlas,k,size_t(t-1)*w+k);equal_pixel(atlas,size_t(k)*w,size_t(k)*w+t-1);}
    // Every internal join and every tile corner shares the same source period.
    for(uint32_t ty=0;ty<4;++ty)for(uint32_t tx=0;tx<4;++tx)
        for(uint32_t k=0;k<t;++k){
            equal_pixel(atlas,size_t(ty*t+k)*w+tx*t,size_t(k)*w);
            equal_pixel(atlas,size_t(ty*t)*w+tx*t+k,k);
            equal_pixel(atlas,size_t(ty*t+k)*w+tx*t+t-1,size_t(k)*w+t-1);
            equal_pixel(atlas,size_t(ty*t+t-1)*w+tx*t+k,size_t(t-1)*w+k);
        }
    std::set<uint16_t> source_heights;
    for(uint32_t row=0;row<8;++row)for(uint32_t col=0;col<4;++col) {
        const uint32_t x=(col*128+64+(row&1)*64)%512;
        const uint32_t y=row*64+32;
        source_heights.insert(atlas.height_r16[size_t(y)*w+x]);
    }
    CHECK(source_heights.size()==16,"default period uses all8shapes times2faces");
    const size_t center=size_t(32)*w+64;
    CHECK(atlas.normal_rg8[2*center]>145&&atlas.normal_rg8[2*center+1]<120,"source UV directional normal reaches brick center");
    CHECK(atlas.normal_rg8[0]==128&&atlas.normal_rg8[1]==128,"mortar neutral normal");
    CHECK(atlas.orm_rgb8[3*center]==255&&atlas.orm_rgb8[3*center+2]==0,"no baked AO/metal classification");
    CHECK(atlas.albedo_rgb8[0]==recipe.mortar_rgb[0],"nominal physical mortar gap clips oversized source rectangle");
    const auto baseline=atlas.content_digest;
    patches[0].recipe_digest++;
    CHECK(castle_bake::build_brick_bond_atlas(patches,recipe,again,error)&&again.content_digest!=baseline,"source recipe provenance changes identity");
    patches=sources();patches[0].texels[0].height_m-=.001f;
    CHECK(castle_bake::build_brick_bond_atlas(patches,recipe,again,error)&&again.content_digest!=baseline,"full finite source bytes enter identity even outside nominal clip");
    const auto recipe_hash=castle_bake::brick_bond_recipe_digest(recipe);
    recipe.brick_rgb[0][0]++;
    CHECK(castle_bake::brick_bond_recipe_digest(recipe)!=recipe_hash,"palette is recipe identity");
    recipe={};patches=sources();
    for(auto& p:patches)for(auto& v:p.texels)v.coverage=0;
    recipe.tile_pixels=16;
    CHECK(castle_bake::build_brick_bond_atlas(patches,recipe,again,error),error.c_str());
    for(size_t i=0;i<again.height_r16.size();++i)CHECK(again.albedo_rgb8[3*i]==recipe.mortar_rgb[0]&&again.normal_rg8[2*i]==128,"zero finite coverage reveals mortar");
    const auto safe_hash=again.content_digest;
    recipe.rows=7;
    CHECK(!castle_bake::build_brick_bond_atlas(patches,recipe,again,error)&&again.content_digest==safe_hash,"odd row count rejects broken half-bond period without changing output");
    recipe.rows=8;patches[0].frame.u={2,0,0};
    CHECK(!castle_bake::build_brick_bond_atlas(patches,recipe,again,error),"scaled source frame rejected");
    patches=sources();patches[0].texels[0].normal_uvn.x=std::numeric_limits<float>::quiet_NaN();
    CHECK(!castle_bake::build_brick_bond_atlas(patches,recipe,again,error),"nonfinite source rejected");
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path=std::filesystem::temp_directory_path()/("brick_bond_atlas_"+std::to_string(stamp)+".gtex");
    CHECK(castle_bake::save_brick_bond_atlas(path.string(),again,error),error.c_str());
    tileset::GTexHeader header;std::vector<uint8_t> alb,nrm,orm;std::vector<uint16_t> hgt;
    CHECK(tileset::load_gtex(path.string(),header,alb,nrm,orm,hgt,error),error.c_str());
    CHECK(header.content_hash==again.header.content_hash&&alb==again.albedo_rgb8&&nrm==again.normal_rg8&&orm==again.orm_rgb8&&hgt==again.height_r16,"existing gtex writer/reader preserves all output channels");
    std::filesystem::remove(path);
    std::printf("brick bond atlas: %d failures\n",g_failures);
    return g_failures?1:0;
}
