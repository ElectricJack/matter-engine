#include "check.h"
#include "animation/animation_targets.h"

#include <cmath>
using namespace matter;using namespace matter::animation;
namespace { SourceSpan src(){return {"ik",1,1,""};} AnimationTransform at(float x){AnimationTransform t;t.translation.x=x;return t;}

RigDefinition two_bone_rig(){RigDefinition rig;rig.joints={{"root","",at(0),1,src()},{"a","root",at(1),1,src()},{"b","a",at(1),1,src()},{"end","b",at(1),1,src()},{"child","end",at(1),1,src()}};return rig;}

// Same chain, but the chain root carries a bind rotation about X so that the
// end joint's PARENT model rotation has a non-zero x component. Without that,
// every quaternion term multiplied by a.x vanishes and an error in one of them
// is unobservable.
RigDefinition tilted_two_bone_rig(float x_radians){
    RigDefinition rig=two_bone_rig();
    const float h=x_radians*0.5f;
    rig.joints[1].local.rotation={std::sin(h),0,0,std::cos(h)};
    return rig;
}

// Compares a model matrix's rotation basis against the basis the quaternion
// should produce (row-major, matching local_matrix in animation_binding_bake).
bool rotation_matches(const Mat4f& model, const Quaternion& q, float tolerance=1e-3f){
    const float xx=q.x*q.x,yy=q.y*q.y,zz=q.z*q.z,xy=q.x*q.y,xz=q.x*q.z,yz=q.y*q.z,
                wx=q.w*q.x,wy=q.w*q.y,wz=q.w*q.z;
    const float expected[9]={1-2*(yy+zz),2*(xy-wz),2*(xz+wy),
                             2*(xy+wz),1-2*(xx+zz),2*(yz-wx),
                             2*(xz-wy),2*(yz+wx),1-2*(xx+yy)};
    const int idx[9]={0,1,2,4,5,6,8,9,10};
    bool ok=true;float worst=0.0f;
    for(int i=0;i<9;++i){
        const float e=std::fabs(model.m[idx[i]]-expected[i]);
        if(e>worst)worst=e;
        if(e>tolerance)ok=false;
    }
    if(!ok)std::printf("  end-effector rotation drift: %.6f\n",worst);
    return ok;
}

void test_chain(){RigDefinition rig=two_bone_rig();Diagnostics d;OzzSkeleton s;CHECK(build_skeleton(rig,s,d),"builds ik chain");JointIndex mid;JointRange range;CHECK(resolve_two_bone_chain(s,1,3,mid,range)&&mid==2,"infers inclusive start/end chain");CHECK(!resolve_two_bone_chain(s,0,3,mid,range),"v1 rejects non-two-segment chain");CanonicalTarget t{};t.chain={1,2,3};t.has_pole=true;t.pole={0,1,0};AnimationTargetState st{};st.evaluated.translation={2,1,0};std::vector<AnimationTransform> locals;for(auto&j:rig.joints)locals.push_back(j.local);std::vector<Mat4f> models;CHECK(local_to_model(s,locals,models),"initial model conversion");CHECK(solve_animation_target(t,s,st,locals,models),"two bone solve updates pose");CHECK(models.size()==locals.size(),"solver returns fresh descendant model matrices");CanonicalTarget other{};other.chain={3,4,4};CHECK(!validate_exclusive_target_chains({t,other}),"overlapping writable chains reject");}

// Regression: the end-effector orientation match composes
// inverse(parent_model_rotation) * target_rotation through a local Hamilton
// product. A wrong term there (e.g. -a.x*b.w instead of -a.x*b.z in the y
// component) still produces a finite, normalized quaternion and a "successful"
// solve, so only asserting the RESULTING orientation catches it. Every other
// quaternion product in the animation tree is checked against this same
// convention.
void test_end_effector_matches_target_orientation(){
    // The tilt gives the end joint's parent a non-zero model-rotation x, and
    // the target below is deliberately NOT a 90-degree turn -- at 90 degrees
    // about Z the quaternion has z == w, which makes a `b.w`-for-`b.z` slip
    // arithmetically invisible.
    RigDefinition rig=tilted_two_bone_rig(1.0472f /* 60 deg */);Diagnostics d;OzzSkeleton s;
    CHECK(build_skeleton(rig,s,d),"orientation fixture builds");
    CanonicalTarget t{};t.chain={1,2,3};t.has_pole=true;t.pole={0,1,0};
    AnimationTargetState st{};
    st.evaluated.translation={2,1,0};
    // Every component distinct and non-zero, normalized.
    {const float x=0.2f,y=0.3f,z=0.5f,w=0.8f;const float n=std::sqrt(x*x+y*y+z*z+w*w);
     st.evaluated.rotation={x/n,y/n,z/n,w/n};}
    st.evaluated_weight=1.0f;
    std::vector<AnimationTransform> locals;for(auto&j:rig.joints)locals.push_back(j.local);
    std::vector<Mat4f> models;
    CHECK(local_to_model(s,locals,models),"orientation fixture model conversion");
    CHECK(solve_animation_target(t,s,st,locals,models),"solve with full weight succeeds");

    CHECK(rotation_matches(models[3],st.evaluated.rotation),
          "end joint model rotation converges to the target orientation at weight 1");

    // A zero-weight target must leave the pose byte-identical: the solver
    // early-outs before touching locals or models.
    std::vector<AnimationTransform> untouched_locals;for(auto&j:rig.joints)untouched_locals.push_back(j.local);
    std::vector<Mat4f> untouched_models;
    CHECK(local_to_model(s,untouched_locals,untouched_models),"zero-weight fixture converts");
    AnimationTargetState off=st;off.evaluated_weight=0.0f;
    const Mat4f before=untouched_models[3];
    CHECK(solve_animation_target(t,s,off,untouched_locals,untouched_models),"zero-weight solve reports success");
    bool unchanged=true;for(int i=0;i<16;++i)if(untouched_models[3].m[i]!=before.m[i])unchanged=false;
    CHECK(unchanged,"zero-weight target leaves the evaluated pose untouched");
}

Float3 rotate(Quaternion q,Float3 v){
    const Float3 u{q.x,q.y,q.z};
    const Float3 uv{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
    const Float3 uuv{u.y*uv.z-u.z*uv.y,u.z*uv.x-u.x*uv.z,u.x*uv.y-u.y*uv.x};
    return {v.x+2*(q.w*uv.x+uuv.x),v.y+2*(q.w*uv.y+uuv.y),v.z+2*(q.w*uv.z+uuv.z)};
}
Float3 translation_of(const Mat4f& m){return {m.m[3],m.m[7],m.m[11]};}

// Solves the standard chain under an arbitrary whole-rig rotation R applied at
// the (zero-translation) root, with the target and pole rotated to match.
// Returns the mid ("elbow") joint's model-space position.
bool solve_under_root_rotation(Quaternion R,Float3 target,Float3 pole,Float3& mid_out){
    RigDefinition rig=two_bone_rig();
    rig.joints[0].local.rotation=R;
    Diagnostics d;OzzSkeleton s;
    if(!build_skeleton(rig,s,d))return false;
    CanonicalTarget t{};t.chain={1,2,3};t.has_pole=true;t.pole=pole;
    AnimationTargetState st{};st.evaluated.translation=target;st.evaluated_weight=1.0f;
    std::vector<AnimationTransform> locals;for(auto&j:rig.joints)locals.push_back(j.local);
    std::vector<Mat4f> models;
    if(!local_to_model(s,locals,models))return false;
    if(!solve_animation_target(t,s,st,locals,models))return false;
    mid_out=translation_of(models[2]);
    return true;
}

// Regression for the pole frame conversion.
//
// CanonicalTarget::pole is animator-root-relative; ozz consumes pole_vector in
// MODEL space. So rotating the whole rig by R and rotating only the (model-space)
// target by R -- while leaving the root-relative pole ALONE -- must rotate the
// whole solution by R.
//
// Two ways to break that, both previously present: converting the pole with the
// inverse root rotation instead of the forward one (a no-op only while the root
// is unrotated), and model_rotation falling back to identity once the root turns
// past 120 degrees. The end joint cannot show either -- the explicit orientation
// match overwrites it regardless -- so this asserts the mid ("elbow") joint,
// whose placement is what the pole actually controls.
void test_pole_conversion_is_equivariant_under_large_root_rotation(){
    const Float3 target{2,1,0},pole{0,1,0};
    Float3 baseline{};
    CHECK(solve_under_root_rotation({0,0,0,1},target,pole,baseline),"baseline solve succeeds");
    // 170 degrees about X: trace = 1 + 2cos(170) < 0, the branch that used to
    // return identity.
    const float a=2.9671f*0.5f;
    const Quaternion R{std::sin(a),0,0,std::cos(a)};
    Float3 rotated{};
    CHECK(solve_under_root_rotation(R,rotate(R,target),pole,rotated),
          "large-root-rotation solve succeeds");
    const Float3 expected=rotate(R,baseline);
    const float drift=std::sqrt((rotated.x-expected.x)*(rotated.x-expected.x)+
                                (rotated.y-expected.y)*(rotated.y-expected.y)+
                                (rotated.z-expected.z)*(rotated.z-expected.z));
    if(drift>1e-3f)std::printf("  elbow equivariance drift: %.6f\n",drift);
    CHECK(drift<=1e-3f,
          "pole conversion stays equivariant when the chain root rotation exceeds the positive-trace branch");
}

// Regression: matrix-to-quaternion must cover all four Shepperd branches.
// trace == 1 + 2*cos(theta), so a parent joint turned past 120 degrees drives
// the trace negative. The original code returned identity there, which made the
// orientation match compose against the wrong frame for exactly the large limb
// rotations targets are most often used for.
void test_orientation_match_survives_large_parent_rotation(){
    for (const float tilt : {2.9671f /* 170 deg, trace < 0 */,
                             2.0944f /* 120 deg, trace ~ 0  */}) {
        RigDefinition rig=tilted_two_bone_rig(tilt);Diagnostics d;OzzSkeleton s;
        CHECK(build_skeleton(rig,s,d),"large-rotation fixture builds");
        CanonicalTarget t{};t.chain={1,2,3};t.has_pole=true;t.pole={0,1,0};
        AnimationTargetState st{};
        st.evaluated.translation={2,1,0};
        {const float x=0.2f,y=0.3f,z=0.5f,w=0.8f;const float n=std::sqrt(x*x+y*y+z*z+w*w);
         st.evaluated.rotation={x/n,y/n,z/n,w/n};}
        st.evaluated_weight=1.0f;
        std::vector<AnimationTransform> locals;for(auto&j:rig.joints)locals.push_back(j.local);
        std::vector<Mat4f> models;
        CHECK(local_to_model(s,locals,models),"large-rotation model conversion");
        CHECK(solve_animation_target(t,s,st,locals,models),"large-rotation solve succeeds");
        CHECK(rotation_matches(models[3],st.evaluated.rotation),
              "orientation match holds when the parent joint rotation exceeds the positive-trace branch");
    }
}
}
// Regression for the AnimatedRigGallery leg defect: a chain whose bind bend
// plane is far from the z axis (the gallery leg's hinge is nearly -x), solved
// to a target it ALREADY satisfies, must be a near-no-op.  Ozz's IKTwoBoneJob
// treats mid_axis's sign as the valid bend side: with the axis inverted the
// current bend reads as "bent backward", the solver picks the mirrored knee
// configuration, and rotate_plane rolls the chain ~180 degrees about the
// start-target axis to restore the pole side.  Every joint POSITION lands
// exactly on the bind pose -- only the chain ROTATIONS carry the half-turn
// roll -- so asserting positions alone can never catch it.
void test_rest_satisfied_target_preserves_chain_rotations(){
    RigDefinition rig;
    const auto at3=[](float x,float y,float z){AnimationTransform t;t.translation={x,y,z};return t;};
    // Gallery leg geometry: thigh and shank nearly straight, bend out of the
    // x-y plane, end below the start.
    rig.joints={{"root","",at3(0,0,0),1,src()},
                {"hip","root",at3(0.4f,1.0f,0.0f),1,src()},
                {"knee","hip",at3(0.05f,-0.95f,0.14f),1,src()},
                {"foot","knee",at3(0.10f,-0.82f,0.40f),1,src()}};
    Diagnostics d;OzzSkeleton s;CHECK(build_skeleton(rig,s,d),"leg fixture builds");
    const Float3 a{0.05f,-0.95f,0.14f}, m{0.10f,-0.82f,0.40f};
    // Ozz convention: mid_axis = cross(mid_end, start_mid), the axis the
    // canonicalizer must derive (animation_validate.cpp).
    Float3 axis{m.y*a.z-m.z*a.y, m.z*a.x-m.x*a.z, m.x*a.y-m.y*a.x};
    const float axis_len=std::sqrt(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z);
    axis={axis.x/axis_len,axis.y/axis_len,axis.z/axis_len};
    CanonicalTarget t{};t.chain={1,2,3};t.bend_axis=axis;t.soften=1.0f;
    // Pole on the bind-plane side so plane alignment is ~zero too.
    t.has_pole=true;t.pole={-0.183f,-0.298f,-0.927f};
    AnimationTargetState st{};st.evaluated.translation={0.55f,-0.77f,0.54f};st.evaluated_weight=1.0f;
    const auto run=[&](const Float3& bend,std::vector<AnimationTransform>& locals,std::vector<Mat4f>& models){
        CanonicalTarget target=t;target.bend_axis=bend;
        locals.clear();for(auto&j:rig.joints)locals.push_back(j.local);
        models.clear();CHECK(local_to_model(s,locals,models),"leg fixture model conversion");
        return solve_animation_target(target,s,st,locals,models);
    };
    std::vector<AnimationTransform> locals;std::vector<Mat4f> models;
    CHECK(run(axis,locals,models),"rest-satisfied solve succeeds");
    bool positions_hold=true;
    const Float3 bind_positions[3]={{0.4f,1.0f,0.0f},{0.45f,0.05f,0.14f},{0.55f,-0.77f,0.54f}};
    for(int k=0;k<3;++k){
        const Mat4f& mm=models[t.chain[k]];
        const float dx=mm.m[3]-bind_positions[k].x,dy=mm.m[7]-bind_positions[k].y,dz=mm.m[11]-bind_positions[k].z;
        if(std::sqrt(dx*dx+dy*dy+dz*dz)>0.05f)positions_hold=false;
    }
    CHECK(positions_hold,"rest-satisfied solve keeps chain joint positions at bind");
    // Bind local rotations are identity, so |w| close to 1 means the solve
    // did not roll the chain.  The mirrored-configuration bug leaves w near
    // zero (a ~180 degree roll) while every position above still passes.
    CHECK(std::fabs(locals[1].rotation.w)>0.99f&&std::fabs(locals[2].rotation.w)>0.99f,
          "rest-satisfied solve leaves hip and knee rotations at bind");
    // The inverted axis must produce the documented failure: exact positions,
    // rolled rotations.  This pins the ozz sign convention itself, so a change
    // in vendored ozz behavior surfaces here instead of as art corruption.
    std::vector<AnimationTransform> flipped_locals;std::vector<Mat4f> flipped_models;
    CHECK(run({-axis.x,-axis.y,-axis.z},flipped_locals,flipped_models),"inverted-axis solve still reports success");
    CHECK(std::fabs(flipped_locals[1].rotation.w)<0.9f||std::fabs(flipped_locals[2].rotation.w)<0.9f,
          "inverted bend axis rolls the chain (ozz bent-side convention)");
}

int main(){test_chain();test_end_effector_matches_target_orientation();test_pole_conversion_is_equivariant_under_large_root_rotation();test_orientation_match_survives_large_parent_rotation();test_rest_satisfied_target_preserves_chain_rotations();return check_summary();}
