#include "animation/animation_binding_bake.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace matter::animation {
namespace {
Float3 add(const Float3&a,const Float3&b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 sub(const Float3&a,const Float3&b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 mul(const Float3&a,float b){return {a.x*b,a.y*b,a.z*b};}
float dot(const Float3&a,const Float3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length2(const Float3&v){return dot(v,v);}
float length(const Float3&v){return std::sqrt(length2(v));}
Quaternion qmul(const Quaternion&a,const Quaternion&b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
Mat4f identity(){Mat4f m{};m.m[0]=m.m[5]=m.m[10]=m.m[15]=1;return m;}
Mat4f multiply(const Mat4f&a,const Mat4f&b){Mat4f r{};for(int row=0;row<4;++row)for(int col=0;col<4;++col)for(int k=0;k<4;++k)r.m[row*4+col]+=a.m[row*4+k]*b.m[k*4+col];return r;}
Mat4f local_matrix(const AnimationTransform&t){Quaternion q=t.rotation;const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);if(n>1e-12f){q.x/=n;q.y/=n;q.z/=n;q.w/=n;}Mat4f m=identity();const float xx=q.x*q.x,yy=q.y*q.y,zz=q.z*q.z,xy=q.x*q.y,xz=q.x*q.z,yz=q.y*q.z,wx=q.w*q.x,wy=q.w*q.y,wz=q.w*q.z;m.m[0]=(1-2*(yy+zz))*t.scale.x;m.m[1]=(2*(xy-wz))*t.scale.y;m.m[2]=(2*(xz+wy))*t.scale.z;m.m[4]=(2*(xy+wz))*t.scale.x;m.m[5]=(1-2*(xx+zz))*t.scale.y;m.m[6]=(2*(yz-wx))*t.scale.z;m.m[8]=(2*(xz-wy))*t.scale.x;m.m[9]=(2*(yz+wx))*t.scale.y;m.m[10]=(1-2*(xx+yy))*t.scale.z;m.m[3]=t.translation.x;m.m[7]=t.translation.y;m.m[11]=t.translation.z;return m;}
Float3 transform_point(const Mat4f&m,const Float3&p){return {m.m[0]*p.x+m.m[1]*p.y+m.m[2]*p.z+m.m[3],m.m[4]*p.x+m.m[5]*p.y+m.m[6]*p.z+m.m[7],m.m[8]*p.x+m.m[9]*p.y+m.m[10]*p.z+m.m[11]};}
bool inverse(const Mat4f&source,Mat4f&out){float a[4][8]{};for(int r=0;r<4;++r)for(int c=0;c<4;++c){a[r][c]=source.m[r*4+c];a[r][c+4]=(r==c)?1.0f:0.0f;}for(int c=0;c<4;++c){int pivot=c;for(int r=c+1;r<4;++r)if(std::fabs(a[r][c])>std::fabs(a[pivot][c]))pivot=r;if(std::fabs(a[pivot][c])<1e-12f)return false;for(int k=0;k<8;++k)std::swap(a[c][k],a[pivot][k]);const float d=a[c][c];for(int k=0;k<8;++k)a[c][k]/=d;for(int r=0;r<4;++r)if(r!=c){const float f=a[r][c];for(int k=0;k<8;++k)a[r][k]-=f*a[c][k];}}for(int r=0;r<4;++r)for(int c=0;c<4;++c)out.m[r*4+c]=a[r][c+4];return true;}
struct BindJoint { Float3 position{}; Quaternion rotation{}; float radius=1; Mat4f world{}; };
std::vector<BindJoint> bind_joints(const CanonicalRig&rig){std::vector<BindJoint> out(rig.joints.size());for(size_t i=0;i<rig.joints.size();++i){const auto&joint=rig.joints[i];const Mat4f local=local_matrix(joint.local);out[i].world=joint.parent==kInvalidJoint?local:multiply(out[joint.parent].world,local);out[i].position=transform_point(out[i].world,{});out[i].radius=joint.radius;const Quaternion local_q=joint.local.rotation;out[i].rotation=joint.parent==kInvalidJoint?local_q:qmul(out[joint.parent].rotation,local_q);}return out;}
float smooth(float q){q=std::max(0.0f,std::min(1.0f,q));return q*q*(3.0f-2.0f*q);}
void add_weight(std::vector<float>&values,JointIndex joint,float weight){if(joint!=kInvalidJoint&&std::isfinite(weight)&&weight>0)values[joint]+=weight;}
VertexInfluences quantize(std::vector<float> values,const std::vector<BindJoint>&joints,const Float3&p){VertexInfluences out;std::vector<JointIndex> order;for(JointIndex i=0;i<values.size();++i)if(values[i]>0)order.push_back(i);if(order.empty()){JointIndex nearest=0;float distance=std::numeric_limits<float>::infinity();for(JointIndex i=0;i<joints.size();++i){const float d=length2(sub(p,joints[i].position));if(d<distance||(d==distance&&i<nearest)){distance=d;nearest=i;}}out.joints[0]=nearest;out.weights[0]=65535;return out;}std::sort(order.begin(),order.end(),[&](JointIndex a,JointIndex b){return values[a]!=values[b]?values[a]>values[b]:a<b;});if(order.size()>kMaxSkinInfluences)order.resize(kMaxSkinInfluences);float total=0;for(auto j:order)total+=values[j];uint32_t assigned=0;for(size_t i=0;i<order.size();++i){out.joints[i]=order[i];if(i==0)continue;const uint32_t q=static_cast<uint32_t>(std::floor(values[order[i]]/total*65535.0f));out.weights[i]=static_cast<uint16_t>(q);assigned+=q;}out.weights[0]=static_cast<uint16_t>(65535u-assigned);return out;}
}

bool BindingClaims::claim(const std::vector<JointIndex>& children,bool decorative){
    std::vector<bool> seen(primary_.size(), false);
    for (JointIndex child : children) {
        if (child >= primary_.size() || seen[child] || (!decorative && primary_[child])) {
            return false;
        }
        seen[child] = true;
    }
    if (!decorative) {
        for (JointIndex child : children) primary_[child] = true;
    }
    return true;
}
bool BindingClaims::claim_skin(const std::vector<JointIndex>& children,bool decorative){return claim(children,decorative);}
bool BindingClaims::claim_rigid(const std::vector<JointIndex>& children,bool decorative){return claim(children,decorative);}
bool validate_attachment(bool child_resolved,bool child_has_committed_animation){return child_resolved&&!child_has_committed_animation;}

bool build_skin_binding(const CanonicalRig&rig,const std::vector<viewer::IndexedPartGeometry>&lods,float falloff_scale,BindingBake&out){out={};if(rig.joints.empty()||!std::isfinite(falloff_scale)||falloff_scale<=0)return false;const auto joints=bind_joints(rig);out.inverse_bind_matrices.resize(joints.size());for(size_t i=0;i<joints.size();++i)if(!inverse(joints[i].world,out.inverse_bind_matrices[i]))return false;for(size_t lod=0;lod<lods.size();++lod){const auto&geometry=lods[lod];if(geometry.vertex_count<0||geometry.vertices.size()!=static_cast<size_t>(geometry.vertex_count)*3)return false;LodSkinBinding baked;baked.indexed_vertex_signature=viewer::indexed_part_geometry_signature(geometry,static_cast<uint32_t>(lod));baked.vertex_count=static_cast<uint32_t>(geometry.vertex_count);baked.influences.reserve(baked.vertex_count);std::vector<bool> used(joints.size(),false);for(uint32_t vertex=0;vertex<baked.vertex_count;++vertex){const Float3 p{geometry.vertices[vertex*3],geometry.vertices[vertex*3+1],geometry.vertices[vertex*3+2]};std::vector<float> values(joints.size(),0.0f);for(JointIndex child=0;child<joints.size();++child){const JointIndex parent=rig.joints[child].parent;if(parent==kInvalidJoint)continue;const Float3 d=sub(joints[child].position,joints[parent].position);const float d2=length2(d);if(d2<=1e-12f)continue;const float t=std::max(0.0f,std::min(1.0f,dot(sub(p,joints[parent].position),d)/d2));const Float3 closest=add(joints[parent].position,mul(d,t));const float radius=joints[parent].radius+(joints[child].radius-joints[parent].radius)*t;const float q=1.0f-length(sub(p,closest))/(falloff_scale*std::max(radius,1e-6f));const float field=smooth(q);add_weight(values,parent,field*(1.0f-t));add_weight(values,child,field*t);}for(JointIndex j=0;j<joints.size();++j){const bool endpoint=rig.joints[j].parent==kInvalidJoint||rig.joints[j].subtree.end==j+1;if(endpoint)add_weight(values,j,smooth(1.0f-length(sub(p,joints[j].position))/(falloff_scale*std::max(joints[j].radius,1e-6f))));}const auto influence=quantize(std::move(values),joints,p);for(size_t k=0;k<kMaxSkinInfluences;++k)if(influence.weights[k])used[influence.joints[k]]=true;baked.influences.push_back(influence);}for(JointIndex j=0;j<joints.size();++j)if(used[j]){JointLocalBounds bounds;bounds.joint=j;bounds.minimum={std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()};bounds.maximum={-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};bool any=false;for(uint32_t vertex=0;vertex<baked.vertex_count;++vertex){bool influences=false;for(size_t k=0;k<kMaxSkinInfluences;++k)if(baked.influences[vertex].joints[k]==j&&baked.influences[vertex].weights[k])influences=true;if(!influences)continue;const Float3 point=transform_point(out.inverse_bind_matrices[j],{geometry.vertices[vertex*3],geometry.vertices[vertex*3+1],geometry.vertices[vertex*3+2]});bounds.minimum.x=std::min(bounds.minimum.x,point.x);bounds.minimum.y=std::min(bounds.minimum.y,point.y);bounds.minimum.z=std::min(bounds.minimum.z,point.z);bounds.maximum.x=std::max(bounds.maximum.x,point.x);bounds.maximum.y=std::max(bounds.maximum.y,point.y);bounds.maximum.z=std::max(bounds.maximum.z,point.z);any=true;}if(any)baked.cluster_bounds.push_back(bounds);}out.lods.push_back(std::move(baked));}return true;}
std::vector<LodBindingSignature> manifest_lod_signatures(const BindingBake&bake){std::vector<LodBindingSignature> out;out.reserve(bake.lods.size());for(const auto&lod:bake.lods)out.push_back({lod.indexed_vertex_signature,lod.vertex_count,static_cast<uint32_t>(lod.influences.size()*kMaxSkinInfluences)});return out;}
bool manifest_matches_binding(const std::vector<LodBindingSignature>&manifest,const BindingBake&bake){return manifest==manifest_lod_signatures(bake);}
} // namespace matter::animation
