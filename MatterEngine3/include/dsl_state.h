#pragma once
#include "raylib.h"   // Vector3, Matrix, Vector4
#include <cstdint>
#include <string>
#include <vector>

namespace dsl {

enum class Session { None, Voxels };  // Triangle/Lattice are later sub-projects.

enum class BrushKind { Sphere, Box };
enum class CsgOp     { Union, Difference, Intersection };

// One authored brush + the op that combines it + the smoothing cursor at emit.
struct BuildOp {
    BrushKind kind;
    CsgOp     op;            // how this brush combines with the accumulated field
    Matrix    transform;     // world transform at emit (transform stack top)
    uint32_t  materialId;    // material cursor at emit
    Vector3   center;        // brush-local center
    float     radius;        // sphere radius
    Vector3   halfExtents;   // box half-extents (unused for sphere)
    float     smoothing;     // smooth-min k cursor at emit
    float     spacing;       // session spacing (resolution floor)
};

// Build buffer: flat op list (resolves the spec open question: flat, not tree).
struct BuildBuffer {
    std::vector<BuildOp> ops;
    void clear() { ops.clear(); }
};

// C++-owned authoring state. JS bindings mutate this; JS holds no engine state.
class DslState {
public:
    DslState();

    // Transform stack
    void pushMatrix();
    void popMatrix();                       // misuse (empty) -> set_error
    void translate(float x, float y, float z);
    void rotateX(float r); void rotateY(float r); void rotateZ(float r);
    void scale(float x, float y, float z);
    void applyMatrix(const float m[16]);    // row-major
    Matrix top() const { return stack_.back(); }

    // Material cursor
    void fill(uint32_t materialId) { material_ = materialId; }
    uint32_t material() const { return material_; }

    // Session enum (one at a time; misuse = error)
    void beginVoxels(float spacing);        // misuse (already open) -> set_error
    void endVoxels();                        // misuse (not open) -> set_error
    Session session() const { return session_; }
    float spacing() const { return spacing_; }

    // Smoothing cursor
    void smoothing(float k) { smoothing_ = (k < 0 ? 0 : k); }
    float smoothing_k() const { return smoothing_; }

    // Brush emission (must be inside a session)
    void sphere(const Vector3& c, float r, CsgOp op);
    void box(const Vector3& c, const Vector3& halfExtents, CsgOp op);

    // Set the op applied to the most-recently-emitted brush (postfix CSG verbs).
    void set_last_op(CsgOp op) {
        if (!buffer_.ops.empty()) buffer_.ops.back().op = op;
        else set_error("CSG op with no preceding brush");
    }

    const BuildBuffer& buffer() const { return buffer_; }

    // Structured error sink (fail-closed). First error wins.
    bool has_error() const { return has_error_; }
    const std::string& error() const { return error_; }
    void set_error(const std::string& m) { if (!has_error_) { has_error_ = true; error_ = m; } }

private:
    std::vector<Matrix> stack_;   // never empty (seeded with identity)
    uint32_t material_ = 0;
    Session  session_ = Session::None;
    float    spacing_ = 0.1f;
    float    smoothing_ = 0.0f;
    size_t   session_start_ = 0;  // index into buffer_.ops where the open session began
    BuildBuffer buffer_;
    bool        has_error_ = false;
    std::string error_;
};

} // namespace dsl
