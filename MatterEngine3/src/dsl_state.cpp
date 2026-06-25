#include "../include/dsl_state.h"
#define RAYMATH_IMPLEMENTATION
#include "raymath.h"

namespace dsl {

DslState::DslState() { stack_.push_back(MatrixIdentity()); }

void DslState::pushMatrix() { stack_.push_back(stack_.back()); }
void DslState::popMatrix() {
    if (stack_.size() <= 1) { set_error("popMatrix without matching pushMatrix"); return; }
    stack_.pop_back();
}
void DslState::translate(float x, float y, float z) {
    stack_.back() = MatrixMultiply(MatrixTranslate(x,y,z), stack_.back());
}
void DslState::rotateX(float r){ stack_.back()=MatrixMultiply(MatrixRotateX(r),stack_.back()); }
void DslState::rotateY(float r){ stack_.back()=MatrixMultiply(MatrixRotateY(r),stack_.back()); }
void DslState::rotateZ(float r){ stack_.back()=MatrixMultiply(MatrixRotateZ(r),stack_.back()); }
void DslState::scale(float x,float y,float z){ stack_.back()=MatrixMultiply(MatrixScale(x,y,z),stack_.back()); }
void DslState::applyMatrix(const float m[16]) {
    Matrix mm = { m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7],
                  m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15] };
    stack_.back() = MatrixMultiply(mm, stack_.back());
}

void DslState::beginVoxels(float spacing) {
    if (session_ != Session::None) { set_error("beginVoxels inside an open session"); return; }
    session_ = Session::Voxels; spacing_ = (spacing > 0 ? spacing : 0.1f);
    session_start_ = buffer_.ops.size();
}
void DslState::endVoxels() {
    if (session_ != Session::Voxels) { set_error("endVoxels with no open voxel session"); return; }
    // Whole-expression smoothing: the spec applies smoothing(k) to the whole
    // session's union, not just the brush emitted while the cursor was set. Stamp
    // the final cursor onto every op emitted in this session so smoothing(k)
    // called anywhere in the build (incl. after the brushes) takes effect.
    for (size_t i = session_start_; i < buffer_.ops.size(); ++i)
        buffer_.ops[i].smoothing = smoothing_;
    session_ = Session::None;
}

void DslState::sphere(const Vector3& c, float r, CsgOp op) {
    if (session_ != Session::Voxels) { set_error("sphere() emitted outside a voxel session"); return; }
    BuildOp o{}; o.kind=BrushKind::Sphere; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.radius=r; o.smoothing=smoothing_; o.spacing=spacing_;
    buffer_.ops.push_back(o);
}
void DslState::box(const Vector3& c, const Vector3& h, CsgOp op) {
    if (session_ != Session::Voxels) { set_error("box() emitted outside a voxel session"); return; }
    BuildOp o{}; o.kind=BrushKind::Box; o.op=op; o.transform=stack_.back();
    o.materialId=material_; o.center=c; o.halfExtents=h; o.smoothing=smoothing_; o.spacing=spacing_;
    buffer_.ops.push_back(o);
}

} // namespace dsl
