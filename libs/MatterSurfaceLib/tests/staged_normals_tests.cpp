// Ordered-field extraction and post-simplification normal regression. Headless.
#include "surface.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
static MtVec3 unit(MtVec3 v) {
    float l = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
    return {v.x/l,v.y/l,v.z/l};
}
static float dot(MtVec3 a, MtVec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static FatPrim shape(int kind) {
    FatPrim f{}; f.kind=kind; f.radius=1; f.halfExtents={1,1,1}; f.boundRadius=2;
    f.invTransform.m[0]=f.invTransform.m[5]=f.invTransform.m[10]=f.invTransform.m[15]=1;
    return f;
}
static MtVec3 normal(SurfaceScratch* s, MtVec3 p, const FieldStages* stages,
                     const std::vector<FatPrim>& fat, float blend=0,
                     Particle* spheres=nullptr, int count=0,
                     Particle* carve=nullptr, int nc=0, float cb=0,
                     Particle* clip=nullptr, int nl=0, MtVec3 fallback={0,0,1}) {
    float xyz[]={p.x,p.y,p.z}, n[]={fallback.x,fallback.y,fallback.z};
    Mesh m{}; m.vertexCount=1; m.vertices=xyz; m.normals=n;
    ComputeSurfaceNormalsStaged(s,&m,spheres,2,count,blend,.02f,stages,
        fat.data(),(int)fat.size(),clip,nl,carve,nc,cb);
    return {n[0],n[1],n[2]};
}
static void oracle(SurfaceScratch* s, MtVec3 p, const FieldStages* stages,
                   const std::vector<FatPrim>& fat, float blend, const char* name,
                   Particle* spheres=nullptr, int count=0, Particle* carve=nullptr,
                   int nc=0, float cb=0) {
    // Independent larger step verifies convergence rather than mirroring the
    // normal implementation's epsilon. Analytic cases below verify direction.
    const float e=.0003f;
    float g[3];
    for (int a=0;a<3;++a) {
        MtVec3 lo=p,hi=p;
        if(a==0){lo.x-=e;hi.x+=e;} if(a==1){lo.y-=e;hi.y+=e;} if(a==2){lo.z-=e;hi.z+=e;}
        float l=ProbeFieldScalar(s,spheres,2,count,blend,stages,fat.data(),(int)fat.size(),carve,nc,cb,lo);
        float h=ProbeFieldScalar(s,spheres,2,count,blend,stages,fat.data(),(int)fat.size(),carve,nc,cb,hi);
        g[a]=(h-l)/(2*e);
    }
    MtVec3 expected=unit({g[0],g[1],g[2]});
    MtVec3 actual=normal(s,p,stages,fat,blend,spheres,count,carve,nc,cb);
    check(dot(expected,actual)>.9995f,name);
}
static void release(Mesh& m) {
    std::free(m.vertices); std::free(m.normals); std::free(m.indices); std::free(m.colors);
}
int main() {
    SurfaceScratch* s=CreateSurfaceScratch();
    FatPrim ell=shape(FAT_PRIM_SPHERE);
    // Rotated nonuniform transform: local x=(world x+y)/sqrt(2)/2,
    // local y=(-world x+y)/sqrt(2), local z=world z/0.6.
    const float c=.70710678118f;
    ell.invTransform.m[0]=c/2; ell.invTransform.m[1]=c/2;
    ell.invTransform.m[4]=-c; ell.invTransform.m[5]=c; ell.invTransform.m[10]=1/.6f;
    MtVec3 p={.7f,.9f,.2f};
    float qx=(p.x+p.y)*c/2,qy=(-p.x+p.y)*c,qz=p.z/.6f;
    MtVec3 expected=unit({c*qx/2-c*qy,c*qx/2+c*qy,qz/.6f});
    check(dot(normal(s,p,nullptr,{ell}),expected)>.99999f,"transformed ellipsoid inverse-transpose gradient");
    oracle(s,p,nullptr,{ell},0,"ellipsoid finite difference oracle");

    FatPrim box=shape(FAT_PRIM_BOX); box.halfExtents={2,.1f,.15f};
    check(dot(normal(s,{.3f,.1f,.04f},nullptr,{box}),{0,1,0})>.99999f,"flat thin beam side stays planar");
    check(dot(normal(s,{.3f,.02f,.15f},nullptr,{box}),{0,0,1})>.99999f,"adjacent hard beam face stays separate");

    FatPrim outer=shape(FAT_PRIM_BOX); outer.halfExtents={1,1,1};
    FatPrim cut=shape(FAT_PRIM_SPHERE); cut.radius=.7f; cut.stage=1;
    FatPrim fill=shape(FAT_PRIM_SPHERE); fill.radius=.2f; fill.stage=2;
    CsgStageOp ops[]={CSG_STAGE_UNION,CSG_STAGE_DIFFERENCE,CSG_STAGE_UNION};
    FieldStages stages{ops,3,nullptr,nullptr,0};
    check(dot(normal(s,{.7f,0,0},&stages,{outer,cut,fill}),{-1,0,0})>.99999f,"carved recess points inward");
    check(dot(normal(s,{.2f,0,0},&stages,{outer,cut,fill}),{1,0,0})>.99999f,"later union restores outward surface");
    oracle(s,{.55f,.3f,.2f},&stages,{outer,cut,fill},.02f,"ordered difference oracle");
    ops[1]=CSG_STAGE_INTERSECTION;
    check(dot(normal(s,{.7f,0,0},&stages,{outer,cut,fill}),{1,0,0})>.99999f,"intersection keeps positive gradient");

    Particle staged_spheres[]={{{0,0,0},1,0},{{0,0,0},.7f,0}};
    int particle_stages[]={0,1};
    CsgStageOp sphere_ops[]={CSG_STAGE_UNION,CSG_STAGE_DIFFERENCE};
    FieldStages sphere_stages{sphere_ops,2,particle_stages,staged_spheres,2};
    check(dot(normal(s,{.7f,0,0},&sphere_stages,{},0,staged_spheres,2),{-1,0,0})>.99999f,
          "ordered sphere-only difference uses staged field gradient");

    FatPrim a=shape(FAT_PRIM_SPHERE), b=a;
    a.invTransform.m[3]=.4f; b.invTransform.m[3]=-.4f;
    oracle(s,{.1f,.9f,.2f},nullptr,{a,b},.15f,"smooth fat union oracle");
    check(std::fabs(normal(s,{0,.9f,0},nullptr,{a,b},.15f).x)<.001f,"symmetric smooth blend has continuous normal");
    Particle sphere{{-.4f,0,0},1,0};
    oracle(s,{.1f,.9f,.2f},nullptr,{b},.15f,"mixed sphere fat blend oracle",&sphere,1);
    Particle carve{{0,0,0},.7f,0};
    oracle(s,{.6f,.2f,.1f},nullptr,{outer},.03f,"legacy trailing smooth carve oracle",nullptr,0,&carve,1,.04f);
    check(dot(normal(s,{.7f,0,0},nullptr,{outer},0,nullptr,0,nullptr,0,0,&carve,1),{-1,0,0})>.99999f,"trailing clip inward gradient");
    check(dot(normal(s,{0,0,0},nullptr,{a,b},.15f,nullptr,0,nullptr,0,0,nullptr,0,{0,3,0}),{0,1,0})>.99999f,"zero gradient normalizes geometric fallback");
    MtVec3 fallback=normal(s,{0,0,0},nullptr,{a,b},.15f,nullptr,0,nullptr,0,0,nullptr,0,{NAN,0,0});
    check(fallback.x==0 && fallback.y==0 && fallback.z==1,"nonfinite fallback is deterministic +Z");

    Bounds bounds{{0,0,0},{3,3,3},4};
    Mesh mesh=GenerateMeshStaged(s,nullptr,2,0,bounds,0,nullptr,&ell,1,nullptr,0,nullptr,0,0);
    check(mesh.vertexCount>0,"pure fat extraction emits mesh");
    bool analytic=true;
    for(int i=0;i<mesh.vertexCount;++i){
        float x=mesh.vertices[3*i],y=mesh.vertices[3*i+1],z=mesh.vertices[3*i+2];
        float lx=(x+y)*c/2,ly=(-x+y)*c,lz=z/.6f;
        MtVec3 want=unit({c*lx/2-c*ly,c*lx/2+c*ly,lz/.6f});
        MtVec3 got={mesh.normals[3*i],mesh.normals[3*i+1],mesh.normals[3*i+2]};
        analytic &= dot(want,got)>.9995f;
    }
    check(analytic,"every extracted ellipsoid normal matches analytic oracle");
    Mesh again=GenerateMeshStaged(s,nullptr,2,0,bounds,0,nullptr,&ell,1,nullptr,0,nullptr,0,0);
    check(mesh.vertexCount==again.vertexCount && mesh.triangleCount==again.triangleCount &&
          std::memcmp(mesh.normals,again.normals,mesh.vertexCount*3*sizeof(float))==0,"staged normals byte deterministic");
    release(mesh);release(again);
    sphere={{0,0,0},1,0};
    Mesh legacy=GenerateMeshWithScratch(s,&sphere,1,1,bounds,.02f,nullptr,0,nullptr,0,0);
    CsgStageOp u=CSG_STAGE_UNION; FieldStages single{&u,1,nullptr,&sphere,1};
    Mesh same=GenerateMeshStaged(s,&sphere,1,1,bounds,.02f,&single,nullptr,0,nullptr,0,nullptr,0,0);
    check(legacy.vertexCount==same.vertexCount && legacy.triangleCount==same.triangleCount &&
          std::memcmp(legacy.vertices,same.vertices,legacy.vertexCount*3*sizeof(float))==0 &&
          std::memcmp(legacy.indices,same.indices,legacy.triangleCount*3*sizeof(unsigned short))==0 &&
          std::memcmp(legacy.normals,same.normals,legacy.vertexCount*3*sizeof(float))==0,"sphere legacy geometry and normals byte parity");
    release(legacy);release(same);DestroySurfaceScratch(s);
    std::printf("staged_normals_tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
