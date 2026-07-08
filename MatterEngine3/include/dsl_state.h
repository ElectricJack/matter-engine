#pragma once
#include "raylib.h"   // Vector3, Matrix, Vector4
#include "dsl_rng.h"
#include "tileset_spec.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tri_emit { class TriangleBuildBuffer; }

namespace dsl {

enum class Session { None, Voxels, Triangles };  // Lattice is a later sub-project.

// Wall-stitch style at interior polyline vertices (the joinType cursor). Mirrors
// tri_emit::JoinType but lives here so dsl_state.h need not include
// triangle_emit.hpp (whose precomp.h float3 clashes with raymath.h). Mapped to
// the tri_emit enum in dsl_triangle.cpp.
enum class JoinKind { Miter, Bevel, Round };

// A finalized 2D cross-section profile captured from a POLYGON beginShape:
// one outer contour (CCW) plus optional hole contours (CW). Retained after
// endShape() so a consumer verb (extrude) can claim it; if unclaimed it flat-
// fills. 2D points only (z ignored) -> no MSL/raylib type dependency here.
struct ProfilePoint2 { float x, y; };
struct RetainedProfile {
    std::vector<ProfilePoint2>              outer;
    std::vector<std::vector<ProfilePoint2>> holes;
    Matrix   transform;        // matrix-stack top captured at endShape()
    uint32_t materialId = 0;
    Vector4  tint{1,1,1,0};
    bool     valid = false;    // a profile is retained and not yet consumed
    bool empty() const { return outer.size() < 3; }
};

// Capsule = segment a->b skinned with radius r0 (sdSegment - r0). Cylinder = capped
// cone a->b with end radii r0/r1 (sdCappedCone; r0==r1 = straight cylinder, r1=0 =
// cone). cone() is sugar that emits a Cylinder with r1<r0.
enum class BrushKind { Sphere, Box, Capsule, Cylinder };
enum class CsgOp     { Union, Difference, Intersection };

// One authored brush + the op that combines it + the smoothing cursor at emit.
struct BuildOp {
    BrushKind kind;
    CsgOp     op;            // how this brush combines with the accumulated field
    Matrix    transform;     // world transform at emit (transform stack top)
    uint32_t  materialId;    // material cursor at emit
    Vector3   center;        // brush-local center (sphere/box) or segment a (capsule/cylinder)
    float     radius;        // sphere radius / capsule radius / cylinder base radius (r0)
    Vector3   halfExtents;   // box half-extents (unused for sphere)
    Vector3   segB;          // segment endpoint b (capsule/cylinder); `center` is endpoint a
    float     r1;            // cylinder/cone top radius (capsule unused; cone r1=0)
    float     smoothing;     // smooth-min k cursor at emit
    float     spacing;       // session spacing (resolution floor)
    Vector4   tint{1,1,1,0}; // tint cursor at emit (alpha = blend strength; w=0 neutral)
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
    ~DslState();

    // Transform stack
    void pushMatrix();
    void popMatrix();                       // misuse (empty) -> set_error
    void translate(float x, float y, float z);
    void rotateX(float r); void rotateY(float r); void rotateZ(float r);
    void scale(float x, float y, float z);
    void applyMatrix(const float m[16]);    // row-major
    // Orient the current frame's +Z (forward) toward `target`, composed onto the
    // current matrix-stack top (G5). `up` defaults to +Y; if it is parallel to the
    // look direction a fallback up is chosen so the basis stays orthonormal.
    void lookAt(float tx, float ty, float tz,
                float upx, float upy, float upz);
    Matrix top() const { return stack_.back(); }
    // Depth of the transform stack (1 == balanced; >1 == leaked pushMatrix). G7.
    size_t stack_depth() const { return stack_.size(); }

    // Current turtle world position: the translation of the matrix-stack top
    // (the origin transformed by it). Ports v2's Vertex()-captures-position so
    // L-system followers can record skeleton vertices in the part's local frame.
    Vector3 position() const {
        Matrix m = stack_.back();
        return Vector3{ m.m12, m.m13, m.m14 };
    }

    // Material cursor
    void fill(uint32_t materialId) { material_ = materialId; }
    uint32_t material() const { return material_; }

    // Tint cursor (G4): RGBA, alpha = blend strength. Default (1,1,1,0) is neutral
    // (alpha 0 = no tint), so unset behavior is byte-identical to before. Captured
    // onto each brush/triangle at emit, mirroring `material_`.
    void tint(float r, float g, float b, float a) { tint_ = Vector4{r,g,b,a}; }
    Vector4 tint() const { return tint_; }

    // Session enum (one at a time; misuse = error)
    void beginVoxels(float spacing);        // misuse (already open) -> set_error
    void endVoxels();                        // misuse (not open) -> set_error
    Session session() const { return session_; }
    float spacing() const { return spacing_; }

    // Smoothing cursor
    void smoothing(float k) { smoothing_ = (k < 0 ? 0 : k); }
    float smoothing_k() const { return smoothing_; }

    // Per-part mesh simplification (keep-fraction): 1.0 = full detail, 0.3 = keep
    // 30% (i.e. "70% simplification"). Parts opt in via this.simplify(); leaves
    // never call it, so their ratio stays 1.0 and the host skips decimation.
    void set_simplify(float r) { simplify_ratio_ = (r <= 0 ? 0.001f : (r > 1 ? 1.0f : r)); }
    float simplify_ratio() const { return simplify_ratio_; }

    // Brush/solid emission. Session-polymorphic (G8):
    //   Voxels    -> SDF brush into the build buffer (unchanged behavior)
    //   None      -> a triangulated solid into the TriangleBuildBuffer (mesh mode)
    //   Triangles -> set_error (a solid is its own primitive, not loose verts)
    void sphere(const Vector3& c, float r, CsgOp op);
    void box(const Vector3& c, const Vector3& halfExtents, CsgOp op);
    // Round primitives (Phase 4 / P2). Session-polymorphic:
    //   Voxels    -> analytic SDF brush (capsule = sdSegment - r; cylinder/cone =
    //                sdCappedCone) into the build buffer.
    //   None      -> set_error (mesh emitters land in Phase 5; a clean handoff,
    //                not a crash).
    //   Triangles -> set_error (a solid is its own primitive, mid-shape misuse).
    // capsule(a,b,r): segment skinned with radius r. cylinder(a,b,r): straight
    // capped cone. cone(a,b,r0,r1): tapered cylinder (sugar; lowers to Cylinder).
    void capsule(const Vector3& a, const Vector3& b, float r, CsgOp op);
    void cylinder(const Vector3& a, const Vector3& b, float r, CsgOp op);
    void cone(const Vector3& a, const Vector3& b, float r0, float r1, CsgOp op);
    // Voxel-session brush emit (the SDF path). Defined in dsl_state.cpp; called by
    // the session dispatch in dsl_triangle.cpp (which owns the triangle buffer).
    void emit_voxel_sphere(const Vector3& c, float r, CsgOp op);
    void emit_voxel_box(const Vector3& c, const Vector3& halfExtents, CsgOp op);
    // a = segment endpoint a, b = endpoint b, r0/r1 = end radii. A capsule passes
    // r1=r0 with kind Capsule; a cylinder/cone passes kind Cylinder.
    void emit_voxel_segment(BrushKind kind, const Vector3& a, const Vector3& b,
                            float r0, float r1, CsgOp op);

    // Direct-triangle (mesh) session. Mutually exclusive with the voxel session;
    // sequential within a part. beginShape opens it, endShape closes it. Vertices
    // are local-space; the current matrix-stack top is captured at beginShape/line
    // and applied to all verts of that primitive. mode: 0=triangles,1=strip,2=fan.
    void beginShape(int mode);              // misuse (voxels open / nested) -> set_error
    void vertex(float x, float y, float z); // misuse (no open shape) -> set_error
    void endShape();                         // misuse (not open) -> set_error
    // A radius-skinned segment (tapered tube of stepped spheres). Standalone:
    // captures the current transform/material, no beginShape needed.
    void line(float ax, float ay, float az, float bx, float by, float bz,
              float r0, float r1);

    // Phase 3 (extrude). POLYGON shape mode (beginShape(3)) is an outline to be
    // filled or swept, distinct from the explicit tessellations 0/1/2. Inside a
    // POLYGON, beginContour()/endContour() push hole contours (Processing-style;
    // outer CCW, holes CW, explicit winding). endShape() on a POLYGON finalizes +
    // RETAINS the contour set as the "current profile" (lazy emission): if a
    // consumer verb claims it before the next beginShape/session change it is
    // swept (no flat face); otherwise it flat-fills (triangulated).
    void beginContour();                     // misuse (not in a POLYGON shape) -> set_error
    void endContour();                       // misuse (no open contour) -> set_error
    // Author-selectable wall stitch at interior polyline vertices, set BEFORE
    // extrude (0=MITER default, 1=BEVEL, 2=ROUND).
    void joinType(int kind) { join_ = (kind==1?JoinKind::Bevel:(kind==2?JoinKind::Round:JoinKind::Miter)); }
    // Sweep the retained POLYGON profile along `path` (a flat array of 3*path_n
    // floats). Voxel session -> error (deferred); mid-open-beginShape -> error;
    // no retained profile -> error. Consumes the profile (suppresses its flat
    // fill). Defined in dsl_triangle.cpp (owns the triangle buffer).
    void extrude(const float* path_xyz, int path_n);
    // Emit the retained profile as a flat filled face IF it was not consumed,
    // then clear it. Called at the next beginShape/session change and at build
    // end (the lazy-emission flush point).
    void flush_retained_profile();

    // Host reads these at bake to register the triangles as one BLAS. Returns null
    // until the buffer is constructed (always in ctor). Pointer-to-incomplete-type
    // is fine for callers that include triangle_emit.hpp (the bake host).
    tri_emit::TriangleBuildBuffer*       triangle_buffer()       { return tris_buf_.get(); }
    const tri_emit::TriangleBuildBuffer* triangle_buffer() const { return tris_buf_.get(); }

    // Set the op applied to the most-recently-emitted brush (postfix CSG verbs).
    // G3: scoped to an OPEN voxel session AND the current session's brush range
    // (session_start_). A stray op across a session boundary (e.g. difference()
    // after endVoxels, or before any brush in this session) is a clear error and
    // can no longer mis-tag a previous session's brush.
    void set_last_op(CsgOp op) {
        if (session_ != Session::Voxels) {
            set_error("CSG op outside an open voxel session");
            return;
        }
        if (buffer_.ops.size() <= session_start_) {
            set_error("CSG op with no preceding brush in this session");
            return;
        }
        buffer_.ops.back().op = op;
    }

    const BuildBuffer& buffer() const { return buffer_; }

    // A recorded child-part placement: resolved hash of the child + the world
    // transform (row-major) at the current matrix-stack top when placeChild ran.
    struct ChildPlacement { uint64_t hash; float transform[16]; };

    // Host installs the declared children's placement table before build();
    // placeChild looks entries up here. Keys come in two forms (the host inserts
    // BOTH for every declared child): a plain `module` name (no-param / fallback,
    // last-variant-wins) and a composite `module \x1f canonical-params-json` that
    // selects ONE specific required variant. Empty map => any placeChild is a
    // fail-closed error.
    void set_child_hashes(std::map<std::string, uint64_t> m) { child_hashes_ = std::move(m); }

    // Exposed composite-key lookup used by both placeChild and j_ts_layer:
    // tries `module \x1f params_json` first (when params_json is non-empty),
    // falls back to plain `module`. Returns true + fills `out` on success.
    // Returns false when the module is not found in the table at all.
    bool lookup_child_hash(const std::string& module,
                           const char* params_json, size_t len,
                           uint64_t& out);

    // Strict composite-key-only lookup: does NOT fall back to the plain module key.
    // Returns true iff `module \x1f params_json` is an explicit entry in the table.
    // Used by layer() to validate that a static params object names a declared variant.
    bool has_composite_child_key(const std::string& module,
                                 const char* params_json, size_t len);

    // Record a placement of `module` at the current transform-stack top.
    //
    // When `params` is present (the JSON.stringify bytes of the placeChild params
    // object), look up the composite `module \x1f params` key so the placement
    // resolves to the EXACT required variant's real resolved hash. Falls back to
    // the plain `module` key when no variant matches (or no params given). Unknown
    // module -> set_error (fail-closed).
    void placeChild(const std::string& module,
                    const void* params = nullptr, size_t params_len = 0);

    const std::vector<ChildPlacement>& children() const { return children_; }

    // Seeded RNG cursor. The host installs a seeded Rng (derived from the part's
    // params) before build(); the bound Math.random() draws from it. Deterministic
    // and process-entropy-free so bakes are reproducible.
    void set_rng(uint64_t seed) { rng_ = std::make_unique<Rng>(seed); }
    Rng* rng() { return rng_.get(); }

    // Structured error sink (fail-closed). First error wins.
    bool has_error() const { return has_error_; }
    const std::string& error() const { return error_; }
    void set_error(const std::string& m) { if (!has_error_) { has_error_ = true; error_ = m; } }

    // Tileset mode: non-null only when evaluating a Tileset root.
    tileset::TilesetState* tileset() { return tileset_.get(); }
    void enable_tileset() { tileset_ = std::make_unique<tileset::TilesetState>(); }
    // Scope bookkeeping for variant(): current sizes of the recorded streams.
    size_t op_count() const { return buffer_.ops.size(); }
    size_t child_count_ts() const { return children_.size(); }

private:
    std::unique_ptr<tileset::TilesetState> tileset_;
    std::vector<Matrix> stack_;   // never empty (seeded with identity)
    uint32_t material_ = 0;
    Vector4  tint_ = Vector4{1,1,1,0};  // G4 tint cursor; (1,1,1,0) = neutral
    Session  session_ = Session::None;
    float    spacing_ = 0.1f;
    float    smoothing_ = 0.0f;
    float    simplify_ratio_ = 1.0f;  // keep-fraction; 1.0 = no simplification
    size_t   session_start_ = 0;  // index into buffer_.ops where the open session began
    BuildBuffer buffer_;
    bool        has_error_ = false;
    std::string error_;
    std::unique_ptr<Rng> rng_;    // seeded by the host before build()
    std::map<std::string, uint64_t>   child_hashes_;   // declared children placement table (see set_child_hashes)
    std::vector<ChildPlacement>       children_;        // accumulated placements
    std::unique_ptr<tri_emit::TriangleBuildBuffer> tris_buf_;  // direct-triangle session

    // Phase 3 POLYGON / extrude state.
    JoinKind        join_ = JoinKind::Miter;     // joinType cursor (default MITER)
    bool            polygon_open_ = false;        // an open beginShape(POLYGON)
    bool            contour_open_ = false;        // an open beginContour inside it
    std::vector<ProfilePoint2>              poly_outer_;   // accumulating outer
    std::vector<std::vector<ProfilePoint2>> poly_holes_;   // accumulating holes
    RetainedProfile retained_;                    // finalized profile (lazy emit)
};

} // namespace dsl
