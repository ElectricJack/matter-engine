#include "../src/sdf_candidate_bounds.h"
#include "surface.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int fails=0;
static void check(bool ok,const char* label){if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);++fails;}}
static FatPrim box(float hx,float hy,float hz){
    FatPrim p{};p.kind=FAT_PRIM_BOX;p.halfExtents={hx,hy,hz};
    p.invTransform.m[0]=p.invTransform.m[5]=p.invTransform.m[10]=p.invTransform.m[15]=1;
    p.boundRadius=std::sqrt(hx*hx+hy*hy+hz*hz);return p;
}
static void release(Mesh& m){std::free(m.vertices);std::free(m.normals);std::free(m.indices);std::free(m.colors);}
int main(){
    mm::Vec3 lo,hi;
    FatPrim long_beam=box(4,.05f,.05f);
    check(sdf_candidates::box_support(long_beam,.01f,lo,hi),"long beam bound available");
    int old_edge=int(std::floor(long_beam.boundRadius*2))-int(std::floor(-long_beam.boundRadius*2))+1;
    int tight=(int(std::floor(hi.x))-int(std::floor(lo.x))+1)*(int(std::floor(hi.y))-int(std::floor(lo.y))+1)*(int(std::floor(hi.z))-int(std::floor(lo.z))+1);
    check(tight==40 && old_edge*old_edge*old_edge==5832,"8m thin beam candidate reduction 5832 to40");
    std::printf("beam candidates: %d -> %d\n",old_edge*old_edge*old_edge,tight);
    FatPrim singular=long_beam;singular.invTransform.m[0]=0;
    check(!sdf_candidates::box_support(singular,.01f,lo,hi),"singular transform uses legacy bounds");
    check(!sdf_candidates::box_support(long_beam,INFINITY,lo,hi),"nonfinite blend uses legacy bounds");
    // Rotated + nonuniformly scaled box: sample expanded local support corners.
    FatPrim rotated=box(1,.1f,.2f);
    const float c=.70710678118f;
    rotated.invTransform.m[0]=c/2;rotated.invTransform.m[1]=c/2;
    rotated.invTransform.m[4]=-c;rotated.invTransform.m[5]=c;
    rotated.invTransform.m[3]=-.3f;rotated.invTransform.m[7]=.1f;
    check(sdf_candidates::box_support(rotated,.01f,lo,hi),"transformed bound available");
    mm::Mat4 fwd;mm::inverse(mm::from_c(rotated.invTransform),fwd);
    float pad=.01f*std::log(192.0f);
    bool contains=true;
    for(int i=0;i<8;++i){
        float q[3]={(i&1?1:-1)*(1+pad),(i&2?1:-1)*(.1f+pad),(i&4?1:-1)*(.2f+pad)};
        float v[3];for(int r=0;r<3;++r)v[r]=fwd.m[r*4]*q[0]+fwd.m[r*4+1]*q[1]+fwd.m[r*4+2]*q[2]+fwd.m[r*4+3];
        contains &= v[0]>=lo.x&&v[0]<=hi.x&&v[1]>=lo.y&&v[1]<=hi.y&&v[2]>=lo.z&&v[2]<=hi.z;
    }
    check(contains,"all transformed smooth support corners contained");

    // Exact production geometry parity: enumerate the original candidate cube,
    // independently mesh every removed cell and require it to be empty. For
    // retained cells compare vertices, indices AND normals from fresh scratches.
    FatPrim beam=box(1.1f,.13f,.17f);
    // Offset off integer boundaries exercises both positive/negative cell floor.
    beam.invTransform.m[3]=-.17f;beam.invTransform.m[7]=-.09f;beam.center={.17f,.09f,0};
    check(sdf_candidates::box_support(beam,.01f,lo,hi),"parity fixture support");
    // Two overlapping union boxes produce a smooth expansion; a trailing
    // difference verifies the bound applies to an ordered expression too.
    FatPrim prims[3]={beam,beam,beam};
    prims[2].halfExtents={.2f,.2f,.07f};prims[2].stage=1;
    CsgStageOp ops[]={CSG_STAGE_UNION,CSG_STAGE_DIFFERENCE};
    FieldStages stages{ops,2,nullptr,nullptr,0};
    SurfaceScratch* old_s=CreateSurfaceScratch();SurfaceScratch* new_s=CreateSurfaceScratch();
    int removed=0,active=0;bool parity=true;
    float inf=beam.boundRadius*2;
    for(int x=int(std::floor(beam.center.x-inf));x<=int(std::floor(beam.center.x+inf));++x)
    for(int y=int(std::floor(beam.center.y-inf));y<=int(std::floor(beam.center.y+inf));++y)
    for(int z=int(std::floor(beam.center.z-inf));z<=int(std::floor(beam.center.z+inf));++z){
        Bounds bounds{{x+.5f,y+.5f,z+.5f},{1,1,1},4};
        Mesh old=GenerateMeshStaged(old_s,nullptr,beam.boundRadius,0,bounds,.01f,&stages,prims,3,nullptr,0,nullptr,0,0);
        bool keep=x>=std::floor(lo.x)&&x<=std::floor(hi.x)&&y>=std::floor(lo.y)&&y<=std::floor(hi.y)&&z>=std::floor(lo.z)&&z<=std::floor(hi.z);
        if(!keep){++removed;parity &= old.vertexCount==0;}
        else{
            Mesh now=GenerateMeshStaged(new_s,nullptr,beam.boundRadius,0,bounds,.01f,&stages,prims,3,nullptr,0,nullptr,0,0);
            parity &= now.vertexCount==old.vertexCount&&now.triangleCount==old.triangleCount;
            if(old.vertexCount){++active;parity &= std::memcmp(now.vertices,old.vertices,old.vertexCount*3*sizeof(float))==0&&std::memcmp(now.indices,old.indices,old.triangleCount*3*sizeof(unsigned short))==0&&std::memcmp(now.normals,old.normals,old.vertexCount*3*sizeof(float))==0;}
            release(now);
        }
        release(old);
    }
    check(parity&&removed>0&&active>0,"discarded cells empty; retained geometry and normals byte-identical");
    DestroySurfaceScratch(old_s);DestroySurfaceScratch(new_s);
    std::printf("candidate geometry parity: removed=%d active=%d; %s\n",removed,active,fails?"FAIL":"PASS");
    return fails?1:0;
}
