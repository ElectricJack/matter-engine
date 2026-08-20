#pragma once


// libs/MatterSurfaceLib/include/tlas_manager.hpp
//
// `TLASManager` -- the CPU-side recorder and builder for a top-level
// acceleration structure (TLAS): the set of *placements* of BLAS geometry.
// Its API is deliberately immediate-mode and modelled on the legacy OpenGL
// matrix stack: push/pop a transform, apply translate/rotate/scale to the top
// of the stack, then `draw(blas_handle, material_id)` to record an instance at
// the current transform. `build()` turns the recorded instances into the BVH
// over instances that the ray-tracing and culling paths consume.
//
// How it fits:
//  - Pairs one-to-one with `blas_manager.hpp`: BLASManager owns the geometry
//    and its bottom-level BVHs, TLASManager owns where that geometry is
//    placed. Both live in MatterSurfaceLib; MatterEngine3's bake pipeline and
//    `part_asset` serialization drive them, and the Vulkan renderer uploads
//    the flattened result.
//  - The BVH / `TLAS` / `BVHInstance` types themselves come from
//    SpatialQueryLib (`bvh.h`, `precomp.h`) -- that library is misnamed and
//    owns the engine's core geometry types, not just queries.
//
// Typical lifecycle:
//     TLASManager tlas(initial_capacity);
//     tlas.ensure_instance_capacity(n);   // once the real count is known
//     tlas.clear();                       // start a fresh recording
//     { ScopedMatrix guard(tlas); tlas.translate(...); tlas.draw(handle, mat); }
//     tlas.build(blas_manager);           // flatten + build the instance BVH
//     tlas.get_tlas(); tlas.get_draw_records();
//
// Considerations:
//  - THE INSTANCE CEILING IS A TRAP. `draw()` refuses past `max_instances_`
//    and reports it with nothing but a `printf`, returning 0. A caller that
//    under-counts silently bakes or renders missing geometry. Call
//    `ensure_instance_capacity()` as soon as the true count is known rather
//    than guessing a constructor argument.
//  - A part is MANY instances, not one. `get_draw_records()` is the
//    authoritative placement list for a part; never assume a single record.
//  - No internal synchronisation: there is no mutex here. Use one manager per
//    part / per worker, and do not record from one thread while another builds
//    or reads.
//  - `instance_storage_` is the backing array and the built `TLAS` holds a RAW
//    pointer into it, so the manager must outlive any use of `get_tlas()`, and
//    a rebuild is required after the storage is reallocated.
//  - Consumers that upload this content must track BOTH `content_revision()`
//    here and `BLASManager::content_revision()`, because TLAS instance records
//    reference BLAS offsets -- see the comment on `content_revision()` below.
//  - Units: transforms are `mm::Mat4` (MathLib, row-major); rotation helpers
//    take RADIANS.

#include "precomp.h"
#include "bvh.h"
#include "matter_math.h"

#include "blas_manager.hpp"
#include "profiler.hpp"
#include <vector>
#include <stack>
#include <memory>
#include <cstdint>

// BVH types are now in global namespace
// float3 and normalize are from global namespace via precomp.h

// Matrix4x4 (row-major float[16], identity-default) has been collapsed onto
// mm::Mat4 (libs/MathLib/include/matter_math.h) -- same layout, same default,
// see docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md Phase 2.

// TLASNode is now available from bvh.h in global namespace

// Unused. Nothing in the tree references this type -- `TLASManager` stores
// `BVHInstance` (from `bvh.h`) instead. Retained from the GPURayTraceExample
// prototype this manager was lifted from; it is not the instance record the
// TLAS is built over.
struct LegacyBVHInstance {
    mat4 transform;
    mat4 invTransform;
    uint32_t instanceId;
    BVH* bvh;
    aabb bounds;
};

// Records instance placements and builds the TLAS over them. Owns its matrix
// stack, its `DrawRecord` list, the flattened `BVHInstance` backing array and
// the built `TLAS` itself; all of it is plain CPU memory (no GPU or OS
// handles), so destruction is cheap and unordered.
//
// Non-copyable, movable. Move is defaulted, which moves the vectors and the
// `unique_ptr<TLAS>` together -- but `instance_array_` and the raw pointer the
// TLAS holds are only valid while the moved-to object's storage is the live
// one, so do not read a `get_tlas()` pointer obtained before a move.
//
// Call order matters: `clear()` -> transform ops + `draw()`/`draw_batch()` ->
// `build(blas_manager)`. `get_tlas()` before the first `build()` returns
// nullptr, and after new `draw()` calls it returns a TLAS that does not
// include them. `content_revision()` is the cheap way to notice both cases.
class TLASManager {
public:
    // One recorded placement: which BLAS, the world transform captured from
    // the top of the matrix stack at `draw()` time, the material override and
    // the instance id assigned by `draw()`. `is_imposter` is NOT set by the
    // constructor (it defaults to false); only the `draw_batch()` path carries
    // it in, from `DrawInstance`.
    // DrawRecord struct for accessing instance data
    struct DrawRecord {
        BLASHandle blas_handle;
        mm::Mat4 transform;
        mm::Mat4 inv_transform;
        uint32_t material_id;
        uint32_t instance_id;
        bool is_imposter = false;

        DrawRecord(BLASHandle handle, const mm::Mat4& trans, uint32_t mat_id, uint32_t inst_id)
            : blas_handle(handle), transform(trans), material_id(mat_id), instance_id(inst_id) {
            // Left as identity: inv_transform is never read. The GPU upload path
            // (tlas_manager.cpp) uses BVHInstance::GetInvTransform() instead. The
            // field is dead — see tech-debt.md §1.
            inv_transform = mm::Mat4();
        }
    };

    explicit TLASManager(int max_instances = 100);
    ~TLASManager();
    
    // Non-copyable but movable
    TLASManager(const TLASManager&) = delete;
    TLASManager& operator=(const TLASManager&) = delete;
    TLASManager(TLASManager&&) = default;
    TLASManager& operator=(TLASManager&&) = default;
    
    // Matrix stack operations - similar to OpenGL matrix stack
    void push_matrix();
    void pop_matrix();
    void load_identity();
    void load_matrix(const mm::Mat4& matrix);
    void multiply_matrix(const mm::Mat4& matrix);
    
    // Transformation convenience functions
    void translate(float x, float y, float z);
    void translate(const float3& translation);
    void scale(float sx, float sy, float sz);
    void scale(float uniform_scale);
    void rotate_x(float angle_radians);
    void rotate_y(float angle_radians);
    void rotate_z(float angle_radians);
    void rotate_axis(const float3& axis, float angle_radians);
    
    // Drawing operations - records instances with current transform
    // Record one instance of `blas_handle` at the current top-of-stack
    // transform. Returns the new instance id, which starts at 1 and increments
    // per recorded instance -- a return of 0 means the call was REFUSED
    // because the instance ceiling was reached (see
    // `ensure_instance_capacity`). The refusal is otherwise reported only by a
    // printf, so check the return value if missing geometry would matter.
    uint32_t draw(BLASHandle blas_handle, uint32_t material_id = 0);
    
    // Batch drawing operations
    struct DrawInstance {
        BLASHandle blas_handle;
        mm::Mat4 transform;
        uint32_t material_id;
        bool is_imposter = false;
    };
    void draw_batch(const std::vector<DrawInstance>& instances);

    // Raise the instance ceiling to at least `n` (never lowers it).
    //
    // draw() REFUSES past the ceiling and reports it with nothing but a printf,
    // so a caller that under-counts silently bakes/renders missing geometry.
    // Callers that only learn their true instance count partway through — e.g.
    // assemble_torus_bvh, which cannot know how many BLAS entries a part
    // contributes until it has loaded the part — should call this once the count
    // is known instead of guessing a constructor argument.
    void ensure_instance_capacity(int n);

    // Also resets the instance-id counter back to 1 and pops the matrix stack
    // back down to a single identity, so a `clear()` is a full recording
    // reset, not just an emptying of the record list. The previously built
    // `TLAS` object is left in place until the next `build()`.
    // Clear all recorded instances (for new frame)
    void clear();
    
    // Flattens every `DrawRecord` into the `BVHInstance` backing array and
    // builds the instance BVH over it. Expensive and allocating -- it is a
    // full rebuild proportional to the instance count, not an incremental
    // update -- so batch all `draw()` calls before calling it. Requires the
    // supplied BLASManager to already hold the geometry the records reference.
    // Build TLAS from recorded instances (call after all draw() calls)
    void build(const BLASManager& blas_manager);
    
    int get_instance_count() const;
    int get_node_count() const;

    // Monotonic counter, bumped whenever the flattened instance/node content
    // changes — draw() records an instance, clear() drops them, build() rebuilds
    // the node array. See BLASManager::content_revision for the rationale; a
    // consumer uploading this content should track BOTH revisions, since the
    // TLAS instance records reference BLAS offsets and go stale when either
    // side moves:
    //
    //     if (tlas.content_revision() == seen_tlas_ &&
    //         blas.content_revision() == seen_blas_) return;  // nothing to do
    uint64_t content_revision() const { return content_revision_; }


    // Access to internal TLAS for visualization
    const TLAS* get_tlas() const { return tlas_.get(); }
    
    // Access to draw records for rasterization
    const std::vector<DrawRecord>& get_draw_records() const { return draw_records_; }

    // Statistics and debugging
    // Currently a NO-OP: the entire body in `src/tlas_manager.cpp` is
    // commented out. Calling it prints nothing.
    void print_stats() const;
    int  get_draw_record_count() const { return static_cast<int>(draw_records_.size()); }
    int  get_matrix_stack_depth() const { return static_cast<int>(matrix_stack_.size()); }

private:
    // Mark data as dirty when TLAS changes
    // Invariant, as in BLASManager: every call site here (draw, clear, build)
    // changes the flattened instance/node content.
    void mark_dirty() const {
        ++content_revision_;
    }
    
    // Get current matrix from top of stack
    const mm::Mat4& get_current_matrix() const;
    mm::Mat4&       get_current_matrix();

    std::stack<mm::Mat4>    matrix_stack_;
    std::vector<DrawRecord>  draw_records_;
    std::unique_ptr<TLAS>    tlas_;
    std::vector<BVHInstance> instance_storage_; // backing array owned by the manager; TLAS holds a raw pointer into it
    std::vector<std::unique_ptr<BVHInstance>> instances_; // Deprecated - kept for compatibility
    BVHInstance*  instance_array_ = nullptr; // Contiguous array for TLAS
    size_t        instance_array_size_ = 0;
    uint32_t      next_instance_id_;
    int           max_instances_;

    // Starts at 1 so a consumer default-initialising its last-seen value to 0
    // always performs its first upload.
    mutable uint64_t content_revision_ = 1;
};

// Utility class for automatic matrix push/pop using RAII
class ScopedMatrix {
public:
    explicit ScopedMatrix(TLASManager& manager) : manager_(manager) {
        manager_.push_matrix();
    }
    
    ~ScopedMatrix() {
        manager_.pop_matrix();
    }

private:
    TLASManager& manager_;
};

// Helper macros for convenient matrix scoping
#define TLAS_PUSH_MATRIX(manager) Performance::ScopedTimer _matrix_scope("Matrix Operations"); ScopedMatrix _matrix_guard(manager)

// Scene building utilities
namespace SceneBuilder {
    // Create a grid of instances
    void create_grid(TLASManager& manager, BLASHandle blas_handle, 
                    int rows, int cols, float spacing, uint32_t material_id = 0);
    
    // Create a circular arrangement of instances
    void create_circle(TLASManager& manager, BLASHandle blas_handle,
                      int count, float radius, uint32_t material_id = 0);
    
    // Create a random scatter of instances
    void create_scatter(TLASManager& manager, BLASHandle blas_handle,
                       int count, float range, uint32_t material_id = 0);
}