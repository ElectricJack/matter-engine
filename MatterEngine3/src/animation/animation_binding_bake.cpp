#include "animation/animation_binding_bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
bool bind_joints(const CanonicalRig&rig, std::vector<BindJoint>&out){
    if (rig.joints.empty() || rig.joints.size() > kMaxJoints) return false;
    // Canonical order is part of the binding contract: a parent must have
    // already been evaluated before a child references it.
    for (size_t i=0;i<rig.joints.size();++i) {
        const JointIndex parent=rig.joints[i].parent;
        if (parent != kInvalidJoint && (parent >= rig.joints.size() || parent >= i)) return false;
    }
    out.resize(rig.joints.size());
    for(size_t i=0;i<rig.joints.size();++i){const auto&joint=rig.joints[i];const Mat4f local=local_matrix(joint.local);out[i].world=joint.parent==kInvalidJoint?local:multiply(out[joint.parent].world,local);out[i].position=transform_point(out[i].world,{});out[i].radius=joint.radius;const Quaternion local_q=joint.local.rotation;out[i].rotation=joint.parent==kInvalidJoint?local_q:qmul(out[joint.parent].rotation,local_q);}return true;
}
float smooth(float q){q=std::max(0.0f,std::min(1.0f,q));return q*q*(3.0f-2.0f*q);}
bool valid_transform(const AnimationTransform& value);
void add_weight(std::vector<float>&values,JointIndex joint,float weight){if(joint!=kInvalidJoint&&std::isfinite(weight)&&weight>0)values[joint]+=weight;}
VertexInfluences quantize(std::vector<float> values,const std::vector<BindJoint>&joints,const Float3&p,const std::vector<bool>&allowed){
    VertexInfluences out; std::vector<JointIndex> order;
    for(JointIndex i=0;i<values.size();++i) if(allowed[i]&&values[i]>0) order.push_back(i);
    if(order.empty()){
        JointIndex nearest=kInvalidJoint; float distance=std::numeric_limits<float>::infinity();
        for(JointIndex i=0;i<joints.size();++i) if(allowed[i]) { const float d=length2(sub(p,joints[i].position)); if(d<distance||(d==distance&&i<nearest)){distance=d;nearest=i;} }
        if(nearest==kInvalidJoint) return out;
        out.joints[0]=nearest;out.weights[0]=65535;return out;
    }
    std::sort(order.begin(),order.end(),[&](JointIndex a,JointIndex b){return values[a]!=values[b]?values[a]>values[b]:a<b;});
    if(order.size()>kMaxSkinInfluences) order.resize(kMaxSkinInfluences);
    float total=0; for(auto j:order) total+=values[j]; uint32_t assigned=0;
    for(size_t i=0;i<order.size();++i){out.joints[i]=order[i];if(i==0)continue;const uint32_t q=static_cast<uint32_t>(std::floor(values[order[i]]/total*65535.0f));out.weights[i]=static_cast<uint16_t>(q);assigned+=q;}
    out.weights[0]=static_cast<uint16_t>(65535u-assigned);return out;
}
void put16(std::vector<uint8_t>& bytes, uint16_t value) { bytes.push_back(uint8_t(value)); bytes.push_back(uint8_t(value >> 8)); }
void put32(std::vector<uint8_t>& bytes, uint32_t value) { for (int i=0;i<4;++i) bytes.push_back(uint8_t(value >> (8*i))); }
void put64(std::vector<uint8_t>& bytes, uint64_t value) { for (int i=0;i<8;++i) bytes.push_back(uint8_t(value >> (8*i))); }
void put_float(std::vector<uint8_t>& bytes, float value) { uint32_t raw=0; std::memcpy(&raw,&value,sizeof raw); put32(bytes,raw); }
void put_string(std::vector<uint8_t>& bytes, const std::string& value) { put32(bytes, static_cast<uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end()); }
void put_transform(std::vector<uint8_t>& bytes, const AnimationTransform& value) {
    put_float(bytes,value.translation.x); put_float(bytes,value.translation.y); put_float(bytes,value.translation.z);
    put_float(bytes,value.rotation.x); put_float(bytes,value.rotation.y); put_float(bytes,value.rotation.z); put_float(bytes,value.rotation.w);
    put_float(bytes,value.scale.x); put_float(bytes,value.scale.y); put_float(bytes,value.scale.z);
}
bool get16(const std::vector<uint8_t>& bytes, size_t& at, uint16_t& value) { if (at>bytes.size() || bytes.size()-at<2) return false; value=uint16_t(bytes[at])|uint16_t(bytes[at+1])<<8; at+=2; return true; }
bool get32(const std::vector<uint8_t>& bytes, size_t& at, uint32_t& value) { if (at>bytes.size() || bytes.size()-at<4) return false; value=0; for(int i=0;i<4;++i)value|=uint32_t(bytes[at++])<<(8*i); return true; }
bool get64(const std::vector<uint8_t>& bytes, size_t& at, uint64_t& value) { if (at>bytes.size() || bytes.size()-at<8) return false; value=0; for(int i=0;i<8;++i)value|=uint64_t(bytes[at++])<<(8*i); return true; }
bool get_float(const std::vector<uint8_t>& bytes, size_t& at, float& value) { uint32_t raw=0; if(!get32(bytes,at,raw))return false; std::memcpy(&value,&raw,sizeof value); return std::isfinite(value); }
bool get_string(const std::vector<uint8_t>& bytes, size_t& at, std::string& value) {
    uint32_t count=0;
    if (!get32(bytes,at,count) || count > 1024 || count > bytes.size()-at) return false;
    value.assign(reinterpret_cast<const char*>(bytes.data()+at), count); at += count;
    return true;
}
bool get_transform(const std::vector<uint8_t>& bytes, size_t& at, AnimationTransform& value) {
    return get_float(bytes,at,value.translation.x) && get_float(bytes,at,value.translation.y) && get_float(bytes,at,value.translation.z) &&
           get_float(bytes,at,value.rotation.x) && get_float(bytes,at,value.rotation.y) && get_float(bytes,at,value.rotation.z) && get_float(bytes,at,value.rotation.w) &&
           get_float(bytes,at,value.scale.x) && get_float(bytes,at,value.scale.y) && get_float(bytes,at,value.scale.z) && valid_transform(value);
}
void tag(std::vector<uint8_t>& bytes, const char (&value)[5]) { bytes.insert(bytes.end(),value,value+4); }
bool take_tag(const std::vector<uint8_t>& bytes, size_t& at, const char (&value)[5]) { if(at>bytes.size() || bytes.size()-at<4 || std::memcmp(bytes.data()+at,value,4)!=0)return false;at+=4;return true; }
bool valid_bounds(const JointLocalBounds& bound, size_t joints) { return bound.joint<joints && std::isfinite(bound.minimum.x)&&std::isfinite(bound.minimum.y)&&std::isfinite(bound.minimum.z)&&std::isfinite(bound.maximum.x)&&std::isfinite(bound.maximum.y)&&std::isfinite(bound.maximum.z)&&bound.minimum.x<=bound.maximum.x&&bound.minimum.y<=bound.maximum.y&&bound.minimum.z<=bound.maximum.z; }
bool valid_transform(const AnimationTransform& value) {
    const float* values[] = {&value.translation.x, &value.translation.y, &value.translation.z,
                             &value.rotation.x, &value.rotation.y, &value.rotation.z, &value.rotation.w,
                             &value.scale.x, &value.scale.y, &value.scale.z};
    for (const float* item : values) if (!std::isfinite(*item)) return false;
    return value.scale.x > 0.0f && value.scale.y > 0.0f && value.scale.z > 0.0f;
}
bool valid_range(const BindingGeometryRange& range) {
    return range.op_begin <= range.op_end && range.triangle_begin <= range.triangle_end &&
           (range.op_begin != range.op_end || range.triangle_begin != range.triangle_end);
}
bool valid_rigid_segment(const RigidSegmentBake& segment, size_t joints) {
    if (segment.name.empty() || segment.name.size() > 1024 || segment.joint >= joints ||
        !valid_transform(segment.bind_offset) || segment.geometry.empty() || segment.geometry.size() > 4096)
        return false;
    for (const auto& range : segment.geometry) if (!valid_range(range)) return false;
    for (const auto& lod : segment.lod_geometry)
        if (lod.triangle_count == 0) return false;
    return true;
}
bool valid_attachment_binding(const AttachmentBake& attachment) {
    return !attachment.name.empty() && attachment.name.size() <= 1024 && !attachment.target.empty() &&
           attachment.target.size() <= 1024 && attachment.child_hash != 0 &&
           (attachment.target_kind == AttachmentTargetKind::Joint || attachment.target_kind == AttachmentTargetKind::Socket) &&
           valid_transform(attachment.local);
}
bool valid_binding(const BindingBake& bake) {
    if (bake.inverse_bind_matrices.empty() || bake.inverse_bind_matrices.size()>kMaxJoints || bake.lods.size()>64) return false;
    if (bake.lods.empty() && bake.rigid_segments.empty() && bake.attachments.empty()) return false;
    for (const auto& matrix : bake.inverse_bind_matrices) for (float value : matrix.m) if (!std::isfinite(value)) return false;
    for (const auto& lod : bake.lods) {
        if(!lod.indexed_vertex_signature || !lod.vertex_count || lod.influences.size()!=lod.vertex_count || lod.clusters.empty()) return false;
        for (const auto& influence : lod.influences) {
            uint32_t total = 0;
            std::vector<JointIndex> joints;
            for(size_t i=0;i<kMaxSkinInfluences;++i) {
                if (!influence.weights[i]) continue;
                if (influence.joints[i]>=bake.inverse_bind_matrices.size() ||
                    std::find(joints.begin(), joints.end(), influence.joints[i]) != joints.end()) return false;
                joints.push_back(influence.joints[i]);
                total += influence.weights[i];
            }
            if (total != 65535u) return false;
        }
        std::vector<uint32_t> cluster_ids;
        std::vector<bool> covered(lod.vertex_count, false);
        for (const auto& cluster : lod.clusters) {
            if (cluster.vertex_begin>=cluster.vertex_end || cluster.vertex_end>lod.vertex_count || cluster.joints.empty() || std::find(cluster_ids.begin(),cluster_ids.end(),cluster.cluster_id)!=cluster_ids.end()) return false;
            cluster_ids.push_back(cluster.cluster_id);
            std::vector<JointIndex> bound_joints;
            for (const auto& bound : cluster.joints) {
                if(!valid_bounds(bound,bake.inverse_bind_matrices.size()) || std::find(bound_joints.begin(),bound_joints.end(),bound.joint)!=bound_joints.end()) return false;
                bound_joints.push_back(bound.joint);
            }
            for (uint32_t vertex=cluster.vertex_begin;vertex<cluster.vertex_end;++vertex) {
                if (covered[vertex]) return false;
                covered[vertex]=true;
                for(size_t i=0;i<kMaxSkinInfluences;++i)
                    if (lod.influences[vertex].weights[i] && std::find(bound_joints.begin(),bound_joints.end(),lod.influences[vertex].joints[i])==bound_joints.end()) return false;
            }
        }
        for (bool vertex : covered) if (!vertex) return false;
    }
    std::vector<std::string> rigid_names;
    for (const auto& segment : bake.rigid_segments) {
        if (!valid_rigid_segment(segment, bake.inverse_bind_matrices.size()) ||
            std::find(rigid_names.begin(), rigid_names.end(), segment.name + "\x1f" + std::to_string(segment.joint)) != rigid_names.end())
            return false;
        rigid_names.push_back(segment.name + "\x1f" + std::to_string(segment.joint));
    }
    if (!bake.lods.empty() || !bake.rigid_segments.empty()) {
        const size_t lod_count = !bake.lods.empty()
            ? bake.lods.size() : bake.rigid_segments.front().lod_geometry.size();
        if (lod_count == 0) return false;
        for (const auto& segment : bake.rigid_segments)
            if (segment.lod_geometry.size() != lod_count) return false;
        const size_t owner_count = (bake.lods.empty() ? 0u : 1u) + bake.rigid_segments.size();
        for (size_t lod_index = 0; lod_index != lod_count; ++lod_index) {
            std::vector<bool> owned(owner_count, false);
            const auto claim = [&owned](uint32_t slot) {
                if (slot >= owned.size() || owned[slot]) return false;
                owned[slot] = true;
                return true;
            };
            if (!bake.lods.empty() && !claim(bake.lods[lod_index].blas_slot)) return false;
            for (const auto& segment : bake.rigid_segments)
                if (!claim(segment.lod_geometry[lod_index].blas_slot)) return false;
            for (bool value : owned) if (!value) return false;
        }
    }
    std::vector<std::string> attachment_names;
    for (const auto& attachment : bake.attachments) {
        if (!valid_attachment_binding(attachment) ||
            std::find(attachment_names.begin(), attachment_names.end(), attachment.name) != attachment_names.end())
            return false;
        attachment_names.push_back(attachment.name);
    }
    return true;
}
const AnimSection* section(const AnimAsset& asset, AnimSectionKind kind) { const AnimSection* found=nullptr; for(const auto& candidate:asset.sections) if(candidate.kind==kind){if(found)return nullptr;found=&candidate;} return found; }
void set_section(AnimAsset& asset, AnimSectionKind kind, std::vector<uint8_t> bytes) { for(auto& section:asset.sections)if(section.kind==kind){section.bytes=std::move(bytes);return;} asset.sections.push_back({kind,std::move(bytes)}); }
}

BindingClaims::BindingClaims(size_t joint_count)
    : primary_(joint_count, false), valid_children_(joint_count, true) {
    // Canonical rigs are preorder-indexed, so index zero is the only root.
    if (!valid_children_.empty()) valid_children_[0] = false;
}

BindingClaims::BindingClaims(const CanonicalRig& rig)
    : primary_(rig.joints.size(), false), valid_children_(rig.joints.size(), false) {
    for (size_t child = 0; child < rig.joints.size(); ++child) {
        const JointIndex parent = rig.joints[child].parent;
        valid_children_[child] = parent != kInvalidJoint && parent < child;
    }
}

bool BindingClaims::claim(const std::vector<JointIndex>& children,bool decorative){
    std::vector<bool> seen(primary_.size(), false);
    for (JointIndex child : children) {
        if (child >= primary_.size() || !valid_children_[child] || seen[child] || (!decorative && primary_[child])) {
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

bool build_skin_binding(const CanonicalRig&rig,const std::vector<JointIndex>&child_joints,const std::vector<viewer::IndexedPartGeometry>&lods,float falloff_scale,BindingBake&out){
    out={};if(!std::isfinite(falloff_scale)||falloff_scale<=0)return false;std::vector<BindJoint> joints;if(!bind_joints(rig,joints))return false;
    std::vector<bool> selected(joints.size(),false), allowed(joints.size(),false);
    for(JointIndex child:child_joints){if(child>=joints.size()||selected[child]||rig.joints[child].parent==kInvalidJoint)return false;selected[child]=true;allowed[child]=true;allowed[rig.joints[child].parent]=true;}
    if(child_joints.empty())return false;
    out.inverse_bind_matrices.resize(joints.size());for(size_t i=0;i<joints.size();++i)if(!inverse(joints[i].world,out.inverse_bind_matrices[i]))return false;
    for(size_t lod=0;lod<lods.size();++lod){const auto&geometry=lods[lod];if(geometry.vertex_count<0||geometry.vertices.size()!=static_cast<size_t>(geometry.vertex_count)*3)return false;LodSkinBinding baked;baked.indexed_vertex_signature=viewer::indexed_part_geometry_signature(geometry,static_cast<uint32_t>(lod));baked.vertex_count=static_cast<uint32_t>(geometry.vertex_count);baked.influences.reserve(baked.vertex_count);std::vector<bool> used(joints.size(),false);
        for(uint32_t vertex=0;vertex<baked.vertex_count;++vertex){const Float3 p{geometry.vertices[vertex*3],geometry.vertices[vertex*3+1],geometry.vertices[vertex*3+2]};std::vector<float> values(joints.size(),0.0f);
            for(JointIndex child=0;child<joints.size();++child){if(!selected[child])continue;const JointIndex parent=rig.joints[child].parent;const Float3 d=sub(joints[child].position,joints[parent].position);const float d2=length2(d);if(d2<=1e-12f)continue;const float t=std::max(0.0f,std::min(1.0f,dot(sub(p,joints[parent].position),d)/d2));const Float3 closest=add(joints[parent].position,mul(d,t));const float radius=joints[parent].radius+(joints[child].radius-joints[parent].radius)*t;const float q=1.0f-length(sub(p,closest))/(falloff_scale*std::max(radius,1e-6f));const float field=smooth(q);add_weight(values,parent,field*(1.0f-t));add_weight(values,child,field*t);}
            for(JointIndex j=0;j<joints.size();++j)if(allowed[j])add_weight(values,j,smooth(1.0f-length(sub(p,joints[j].position))/(falloff_scale*std::max(joints[j].radius,1e-6f))));
            const auto influence=quantize(std::move(values),joints,p,allowed);for(size_t k=0;k<kMaxSkinInfluences;++k)if(influence.weights[k])used[influence.joints[k]]=true;baked.influences.push_back(influence);}
        ClusterJointBounds cluster;cluster.cluster_id=0;cluster.vertex_begin=0;cluster.vertex_end=baked.vertex_count;for(JointIndex j=0;j<joints.size();++j)if(used[j]){JointLocalBounds bounds;bounds.joint=j;bounds.minimum={std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()};bounds.maximum={-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};bool any=false;for(uint32_t vertex=0;vertex<baked.vertex_count;++vertex){bool influences=false;for(size_t k=0;k<kMaxSkinInfluences;++k)if(baked.influences[vertex].joints[k]==j&&baked.influences[vertex].weights[k])influences=true;if(!influences)continue;const Float3 point=transform_point(out.inverse_bind_matrices[j],{geometry.vertices[vertex*3],geometry.vertices[vertex*3+1],geometry.vertices[vertex*3+2]});bounds.minimum.x=std::min(bounds.minimum.x,point.x);bounds.minimum.y=std::min(bounds.minimum.y,point.y);bounds.minimum.z=std::min(bounds.minimum.z,point.z);bounds.maximum.x=std::max(bounds.maximum.x,point.x);bounds.maximum.y=std::max(bounds.maximum.y,point.y);bounds.maximum.z=std::max(bounds.maximum.z,point.z);any=true;}if(any)cluster.joints.push_back(bounds);}if(!cluster.joints.empty())baked.clusters.push_back(std::move(cluster));out.lods.push_back(std::move(baked));}
    return true;
}
std::vector<LodBindingSignature> manifest_lod_signatures(const BindingBake&bake){std::vector<LodBindingSignature> out;out.reserve(bake.lods.size());for(const auto&lod:bake.lods)out.push_back({lod.indexed_vertex_signature,lod.vertex_count,static_cast<uint32_t>(lod.influences.size()*kMaxSkinInfluences)});return out;}
bool manifest_matches_binding(const std::vector<LodBindingSignature>&manifest,const BindingBake&bake){return manifest==manifest_lod_signatures(bake);}

bool set_anim_binding_bake(AnimAsset& asset, const BindingBake& bake) {
    if (!valid_binding(bake)) return false;
    std::vector<uint8_t> geometry, inverse_binds, bounds, rigid_segments, attachments;
    tag(geometry,"GBND"); put32(geometry,2); put32(geometry,static_cast<uint32_t>(bake.lods.size()));
    for (const auto& lod : bake.lods) {
        put64(geometry,lod.indexed_vertex_signature); put32(geometry,lod.vertex_count); put32(geometry,lod.blas_slot); put32(geometry,static_cast<uint32_t>(lod.influences.size()));
        for (const auto& influence : lod.influences) for (size_t i=0;i<kMaxSkinInfluences;++i) { put16(geometry,influence.joints[i]); put16(geometry,influence.weights[i]); }
    }
    tag(inverse_binds,"IBND"); put32(inverse_binds,1); put32(inverse_binds,static_cast<uint32_t>(bake.inverse_bind_matrices.size()));
    for (const auto& matrix : bake.inverse_bind_matrices) for (float value : matrix.m) put_float(inverse_binds,value);
    tag(bounds,"CBND"); put32(bounds,1); put32(bounds,static_cast<uint32_t>(bake.lods.size()));
    for (const auto& lod : bake.lods) {
        put32(bounds,static_cast<uint32_t>(lod.clusters.size()));
        for (const auto& cluster : lod.clusters) {
            put32(bounds,cluster.cluster_id); put32(bounds,cluster.vertex_begin); put32(bounds,cluster.vertex_end); put32(bounds,static_cast<uint32_t>(cluster.joints.size()));
            for (const auto& joint : cluster.joints) { put16(bounds,joint.joint); put_float(bounds,joint.minimum.x); put_float(bounds,joint.minimum.y); put_float(bounds,joint.minimum.z); put_float(bounds,joint.maximum.x); put_float(bounds,joint.maximum.y); put_float(bounds,joint.maximum.z); }
        }
    }
    tag(rigid_segments,"RBND"); put32(rigid_segments,2); put32(rigid_segments,static_cast<uint32_t>(bake.rigid_segments.size()));
    for (const auto& segment : bake.rigid_segments) {
        put_string(rigid_segments,segment.name); put16(rigid_segments,segment.joint);
        rigid_segments.push_back(segment.decorative ? 1 : 0); put_transform(rigid_segments,segment.bind_offset);
        put32(rigid_segments,static_cast<uint32_t>(segment.geometry.size()));
        for (const auto& range : segment.geometry) { put32(rigid_segments,range.op_begin); put32(rigid_segments,range.op_end); put32(rigid_segments,range.triangle_begin); put32(rigid_segments,range.triangle_end); }
        put32(rigid_segments,static_cast<uint32_t>(segment.lod_geometry.size()));
        for (const auto& lod : segment.lod_geometry) { put32(rigid_segments,lod.blas_slot); put32(rigid_segments,lod.triangle_count); }
    }
    tag(attachments,"ABND"); put32(attachments,1); put32(attachments,static_cast<uint32_t>(bake.attachments.size()));
    for (const auto& attachment : bake.attachments) {
        put_string(attachments,attachment.name); attachments.push_back(static_cast<uint8_t>(attachment.target_kind));
        put_string(attachments,attachment.target); put64(attachments,attachment.child_hash); put_transform(attachments,attachment.local);
    }
    set_section(asset,AnimSectionKind::GeometryBindings,std::move(geometry));
    set_section(asset,AnimSectionKind::InverseBindMatrices,std::move(inverse_binds));
    set_section(asset,AnimSectionKind::ClusterBounds,std::move(bounds));
    set_section(asset,AnimSectionKind::RigidSegments,std::move(rigid_segments));
    set_section(asset,AnimSectionKind::Attachments,std::move(attachments));
    return true;
}

bool get_anim_binding_bake(const AnimAsset& asset, BindingBake& bake) {
    bake={}; const auto* geometry=section(asset,AnimSectionKind::GeometryBindings); const auto* inverse_binds=section(asset,AnimSectionKind::InverseBindMatrices); const auto* bounds=section(asset,AnimSectionKind::ClusterBounds); const auto* rigid_segments=section(asset,AnimSectionKind::RigidSegments); const auto* attachments=section(asset,AnimSectionKind::Attachments);
    if(!geometry||!inverse_binds||!bounds||!rigid_segments||!attachments)return false;
    size_t at=0; uint32_t version=0,lod_count=0;
    if(!take_tag(geometry->bytes,at,"GBND")||!get32(geometry->bytes,at,version)||version!=2||!get32(geometry->bytes,at,lod_count)||lod_count>64)return false;
    bake.lods.resize(lod_count);
    for(auto& lod:bake.lods){uint32_t influence_count=0;if(!get64(geometry->bytes,at,lod.indexed_vertex_signature)||!get32(geometry->bytes,at,lod.vertex_count)||!get32(geometry->bytes,at,lod.blas_slot)||!get32(geometry->bytes,at,influence_count)||influence_count!=lod.vertex_count||influence_count>(geometry->bytes.size()-at)/(kMaxSkinInfluences*4))return false;lod.influences.resize(influence_count);for(auto& influence:lod.influences)for(size_t i=0;i<kMaxSkinInfluences;++i)if(!get16(geometry->bytes,at,influence.joints[i])||!get16(geometry->bytes,at,influence.weights[i]))return false;}
    if(at!=geometry->bytes.size())return false;
    at=0; uint32_t matrix_count=0;if(!take_tag(inverse_binds->bytes,at,"IBND")||!get32(inverse_binds->bytes,at,version)||version!=1||!get32(inverse_binds->bytes,at,matrix_count)||matrix_count==0||matrix_count>kMaxJoints)return false;bake.inverse_bind_matrices.resize(matrix_count);for(auto& matrix:bake.inverse_bind_matrices)for(float& value:matrix.m)if(!get_float(inverse_binds->bytes,at,value))return false;if(at!=inverse_binds->bytes.size())return false;
    at=0; uint32_t bound_lods=0;if(!take_tag(bounds->bytes,at,"CBND")||!get32(bounds->bytes,at,version)||version!=1||!get32(bounds->bytes,at,bound_lods)||bound_lods!=lod_count)return false;
    for(auto& lod:bake.lods){uint32_t cluster_count=0;if(!get32(bounds->bytes,at,cluster_count)||cluster_count>lod.vertex_count||cluster_count>(bounds->bytes.size()-at)/16)return false;lod.clusters.resize(cluster_count);for(auto& cluster:lod.clusters){uint32_t joint_count=0;if(!get32(bounds->bytes,at,cluster.cluster_id)||!get32(bounds->bytes,at,cluster.vertex_begin)||!get32(bounds->bytes,at,cluster.vertex_end)||!get32(bounds->bytes,at,joint_count)||joint_count>matrix_count||joint_count>(bounds->bytes.size()-at)/26)return false;cluster.joints.resize(joint_count);for(auto& joint:cluster.joints){if(!get16(bounds->bytes,at,joint.joint)||!get_float(bounds->bytes,at,joint.minimum.x)||!get_float(bounds->bytes,at,joint.minimum.y)||!get_float(bounds->bytes,at,joint.minimum.z)||!get_float(bounds->bytes,at,joint.maximum.x)||!get_float(bounds->bytes,at,joint.maximum.y)||!get_float(bounds->bytes,at,joint.maximum.z))return false;}}}
    if (at != bounds->bytes.size()) return false;
    at=0; uint32_t rigid_count=0;
    if(!take_tag(rigid_segments->bytes,at,"RBND")||!get32(rigid_segments->bytes,at,version)||version!=2||!get32(rigid_segments->bytes,at,rigid_count)||rigid_count>kMaxJoints)return false;
    bake.rigid_segments.resize(rigid_count);
    for (auto& segment : bake.rigid_segments) {
        uint8_t decorative=0; uint32_t range_count=0;
        if(!get_string(rigid_segments->bytes,at,segment.name)||!get16(rigid_segments->bytes,at,segment.joint)||at>=rigid_segments->bytes.size())return false;
        decorative=rigid_segments->bytes[at++]; if(decorative>1||!get_transform(rigid_segments->bytes,at,segment.bind_offset)||!get32(rigid_segments->bytes,at,range_count)||range_count==0||range_count>4096||range_count>(rigid_segments->bytes.size()-at)/16)return false;
        segment.decorative=decorative!=0; segment.geometry.resize(range_count);
        for (auto& range : segment.geometry) if(!get32(rigid_segments->bytes,at,range.op_begin)||!get32(rigid_segments->bytes,at,range.op_end)||!get32(rigid_segments->bytes,at,range.triangle_begin)||!get32(rigid_segments->bytes,at,range.triangle_end))return false;
        uint32_t lod_count_for_segment=0;
        if(!get32(rigid_segments->bytes,at,lod_count_for_segment)||lod_count_for_segment>64||lod_count_for_segment>(rigid_segments->bytes.size()-at)/8)return false;
        segment.lod_geometry.resize(lod_count_for_segment);
        for (auto& lod : segment.lod_geometry) if(!get32(rigid_segments->bytes,at,lod.blas_slot)||!get32(rigid_segments->bytes,at,lod.triangle_count))return false;
    }
    if(at!=rigid_segments->bytes.size())return false;
    at=0; uint32_t attachment_count=0;
    if(!take_tag(attachments->bytes,at,"ABND")||!get32(attachments->bytes,at,version)||version!=1||!get32(attachments->bytes,at,attachment_count)||attachment_count>kMaxJoints)return false;
    bake.attachments.resize(attachment_count);
    for (auto& attachment : bake.attachments) {
        uint8_t kind=0;
        if(!get_string(attachments->bytes,at,attachment.name)||at>=attachments->bytes.size())return false;
        kind=attachments->bytes[at++]; if(kind>static_cast<uint8_t>(AttachmentTargetKind::Socket)||!get_string(attachments->bytes,at,attachment.target)||!get64(attachments->bytes,at,attachment.child_hash)||!get_transform(attachments->bytes,at,attachment.local))return false;
        attachment.target_kind=static_cast<AttachmentTargetKind>(kind);
    }
    return at==attachments->bytes.size()&&valid_binding(bake);
}
} // namespace matter::animation
