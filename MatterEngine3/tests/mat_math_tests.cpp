// CPU regression tests for the row-major normal transform used by both subtree
// flattening and world ray queries. No renderer/device or asset cache required.
#include "mat_math.h"
#include "check.h"
#include <array>
#include <cmath>

namespace {
using Vec = std::array<float, 3>;
using Mat = std::array<float, 16>;
float dot(const Vec& a, const Vec& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
Vec cross(const Vec& a, const Vec& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
Vec normalized(Vec v) {
    const float length = std::sqrt(dot(v,v));
    if (length > 1e-12f) for (float& x : v) x /= length;
    return v;
}
Vec direction(const Mat& m, const Vec& v) {
    return {m[0]*v[0]+m[1]*v[1]+m[2]*v[2],
            m[4]*v[0]+m[5]*v[1]+m[6]*v[2],
            m[8]*v[0]+m[9]*v[1]+m[10]*v[2]};
}
Vec normal(const Mat& m, const Vec& v) {
    Vec out{};
    NormalMat(m.data()).apply(v.data(), out.data());
    return out;
}
void expect(const Vec& got, const Vec& expected, const char* message) {
    CHECK(std::fabs(got[0]-expected[0]) < 2e-6f &&
          std::fabs(got[1]-expected[1]) < 2e-6f &&
          std::fabs(got[2]-expected[2]) < 2e-6f, message);
}
float determinant(const Mat& m) {
    return dot(Vec{m[0],m[4],m[8]}, cross(Vec{m[1],m[5],m[9]},Vec{m[2],m[6],m[10]}));
}
void plane(const Mat& m, const Vec& u, const Vec& v, const char* message) {
    const Vec transformedU = direction(m,u), transformedV = direction(m,v);
    const Vec n = normal(m, cross(u,v));
    CHECK(std::fabs(dot(n, normalized(transformedU))) < 2e-6f &&
          std::fabs(dot(n, normalized(transformedV))) < 2e-6f, message);
    Vec geometric = normalized(cross(transformedU,transformedV));
    // A normal is a covector. Mirroring reverses triangle winding, but must
    // not arbitrarily flip an authored outward normal a second time.
    if (determinant(m) < 0) for (float& x : geometric) x = -x;
    expect(n, geometric, message);
    CHECK(std::fabs(dot(n,n)-1.f) < 2e-6f, "transformed normals are unit length");
}
Mat identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
}

int main() {
    const Vec diagonal = normalized(Vec{1,2,3});
    Mat translation = identity();
    translation[3]=14; translation[7]=-6; translation[11]=27;
    expect(normal(translation,diagonal),diagonal,"translation has no effect on normals");

    const Mat shear = {1,0,1,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    expect(normal(shear,{1,0,0}),normalized(Vec{1,0,-1}),
           "XZ shear maps +X normal to normalized (1,0,-1), not the inverse's +X");
    plane(shear,{0,1,0},{0,0,1},"sheared YZ face normal remains perpendicular to both edges");

    constexpr float c=.8660254037844386f, s=.5f;
    const Mat rotation = {c,0,s,0, 0,1,0,0, -s,0,c,0, 0,0,0,1};
    expect(normal(rotation,{1,0,0}),Vec{c,0,-s},"pure yaw rotates normals in the same direction as geometry");
    const Mat scale = {3,0,0,0, 0,2,0,0, 0,0,.4f,0, 0,0,0,1};
    Mat rotatedScale{};
    mul16(rotation.data(),scale.data(),rotatedScale.data());
    expect(normal(rotatedScale,{1,2,3}),normalized(direction(rotation,Vec{1.f/3,1,7.5f})),
           "nonuniform scale uses reciprocal axes before forward rotation");
    plane(rotatedScale,{2,-1,0},{3,0,-1},"rotated nonuniform stock instance preserves plane perpendicularity");

    // Off-diagonal terms in every axis expose transposes that diagonal-only
    // scale tests miss. Include reflection and translation in the same chain.
    const Mat affine = {2,.4f,1,12, .3f,3,-.2f,-5, .6f,.1f,.7f,8, 0,0,0,1};
    const Mat reflection = {-2,.3f,.6f,0, 0,1,.2f,0, 0,0,.5f,0, 0,0,0,1};
    for (const Mat& m : {affine, reflection, rotatedScale}) {
        plane(m,{1,0,0},{0,1,0},"affine XY plane");
        plane(m,{0,1,0},{0,0,1},"affine YZ plane");
        plane(m,{0,0,1},{1,0,0},"affine ZX plane");
        plane(m,{1,2,-1},{-.5f,1,3},"affine oblique plane");
    }
    Mat composed{};
    mul16(affine.data(),rotatedScale.data(),composed.data());
    expect(normal(composed,diagonal),normal(affine,normal(rotatedScale,diagonal)),
           "hierarchical transforms compose normal covectors in the same order");

    // Preserve the established singular policy; this change only corrects the
    // invertible branch, not degeneracy/zero-vector behavior.
    const Mat singular = {0,0,1,0, 0,2,0,0, 0,0,3,0, 0,0,0,1};
    expect(normal(singular,{1,1,1}),normalized(Vec{1,2,3}),"singular fallback remains raw 3x3");
    expect(normal(singular,{1,0,0}),Vec{0,0,0},"collapsed normal remains a finite zero vector");
    expect(normal(shear,{0,0,0}),Vec{0,0,0},"zero normal remains finite");
    std::puts("mat_math_tests: row-major normal transforms, shear and affine plane perpendicularity");
    return check_summary();
}
