// libs/MatterSurfaceLib/src/tlas_manager.cpp
//
// Implementation of `TLASManager` — the scene-graph-shaped front end to
// SpatialQueryLib's top-level acceleration structure. The class contract lives
// in `../include/tlas_manager.hpp`; this file is the mechanism.
//
// Model. The manager owns an OpenGL-style matrix stack (`push_matrix` /
// `translate` / `rotate_*` / `scale`, with `ScopedMatrix` and the
// `TLAS_PUSH_MATRIX` macro for RAII scoping). `draw(blas_handle, material_id)`
// snapshots the CURRENT top-of-stack matrix into a `DrawRecord` and returns a
// nonzero instance id. `build(blas_manager)` then converts every record into a
// `BVHInstance` and constructs the `TLAS`.
//
//   TLASManager tlas(max_instances);
//   { TLAS_PUSH_MATRIX(tlas); tlas.translate(...); tlas.draw(h, mat); }
//   tlas.build(blas);          // required before get_tlas()/get_instance_count()
//   tlas.clear();              // start the next frame/bake
//
// Ownership and lifetime. `instance_storage_` is the manager-owned backing
// array the `TLAS` holds a RAW pointer into, so the built `TLAS` must be
// dropped before that vector is cleared or reallocated — `build()` does exactly
// that, and any future edit must preserve the order. `instances_` (the
// unique_ptr vector) is a deprecated parallel copy that `build()` still fills;
// the `TLAS` does not point at it.
//
// This file is CPU-only — no Vulkan, no GL, no descriptor or buffer handling.
// The GPU upload path consumes `get_draw_records()` / `get_tlas()` elsewhere and
// polls `content_revision()` to know when to re-upload.
//
// Threading. No locking anywhere; a manager belongs to whichever thread is
// building or drawing with it. Warnings go to `printf`, not the engine logger,
// and a capacity refusal in `draw()` is reported ONLY by that printf.

#include "../include/tlas_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

TLASManager::TLASManager(int max_instances)
    : tlas_(nullptr), next_instance_id_(1), max_instances_(max_instances) {

    // Initialize matrix stack with identity
    matrix_stack_.push(mm::identity());
    
    // Reserve space for draw records
    draw_records_.reserve(max_instances);
}

// `instance_storage_`, `instances_` and `tlas_` clean themselves up, so there
// is nothing to do here. Kept out-of-line (rather than `= default` in the
// header) so `unique_ptr<TLAS>` only needs the complete type in this TU.
TLASManager::~TLASManager() = default;

const mm::Mat4& TLASManager::get_current_matrix() const {
    return matrix_stack_.top();
}

mm::Mat4& TLASManager::get_current_matrix() {
    return const_cast<mm::Mat4&>(matrix_stack_.top());
}

// Duplicate the top of the matrix stack. Depth is capped at 32; past that the
// push is SKIPPED with only a printf and recorded in `suppressed_pushes_`, so
// the matching `pop_matrix` (or `ScopedMatrix` destructor) is refused too and
// the caller's outer level survives. The over-deep nesting level is still lost:
// anything drawn inside it uses the level-32 transform. Keep nesting shallow,
// or hoist the transform.
void TLASManager::push_matrix() {
    if (matrix_stack_.size() >= 32) { // Reasonable limit
        printf("Warning: Matrix stack overflow in TLAS manager\n");
        // Remember the refusal so the matching pop_matrix() is refused too.
        ++suppressed_pushes_;
        return;
    }

    matrix_stack_.push(matrix_stack_.top());
}

void TLASManager::pop_matrix() {
    // Unwind a refused push first: popping here would remove a level this
    // pop's caller never pushed, quietly replacing an OUTER transform with an
    // inner one instead of merely dropping the over-deep nesting.
    if (suppressed_pushes_ > 0) {
        --suppressed_pushes_;
        return;
    }
    if (matrix_stack_.size() <= 1) {
        printf("Warning: Matrix stack underflow in TLAS manager\n");
        return;
    }

    matrix_stack_.pop();
}

void TLASManager::load_identity() {
    get_current_matrix() = mm::identity();
}

void TLASManager::load_matrix(const mm::Mat4& matrix) {
    get_current_matrix() = matrix;
}

void TLASManager::multiply_matrix(const mm::Mat4& matrix) {
    mm::Mat4& current = get_current_matrix();
    current = mm::multiply(current, matrix);
}

void TLASManager::translate(float x, float y, float z) {
    mm::Mat4 trans = mm::translation(mm::Vec3{x, y, z});
    multiply_matrix(trans);
}

void TLASManager::translate(const float3& translation) {
    translate(translation.x, translation.y, translation.z);
}

void TLASManager::scale(float sx, float sy, float sz) {
    mm::Mat4 scale_matrix = mm::scale(mm::Vec3{sx, sy, sz});
    multiply_matrix(scale_matrix);
}

void TLASManager::scale(float uniform_scale) {
    scale(uniform_scale, uniform_scale, uniform_scale);
}

void TLASManager::rotate_x(float angle_radians) {
    mm::Mat4 rot = mm::rotation_x(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_y(float angle_radians) {
    mm::Mat4 rot = mm::rotation_y(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_z(float angle_radians) {
    mm::Mat4 rot = mm::rotation_z(angle_radians);
    multiply_matrix(rot);
}

void TLASManager::rotate_axis(const float3& axis, float angle_radians) {
    // mm::rotation_axis requires a pre-normalized axis (returns identity()
    // otherwise); the deleted matrix_rotation_axis() normalized internally,
    // so normalize explicitly here to preserve that lenient behaviour for
    // any (currently nonexistent, but public-API) caller passing a
    // non-unit axis.
    mm::Vec3 axis_mm{axis.x, axis.y, axis.z};
    mm::Mat4 rot = mm::rotation_axis(mm::normalize(axis_mm), angle_radians);
    multiply_matrix(rot);
}

// Record one instance of `blas_handle` at the current top-of-stack transform.
// Returns the new instance id, which is always >= 1, so a 0 return means the
// record was REFUSED — either the handle was invalid or the manager is at its
// instance ceiling (raise it with `ensure_instance_capacity`). The refusal is
// otherwise silent apart from a printf, so a caller that ignores the return
// value renders/bakes missing geometry. Does not build anything; call `build()`
// once all draws are recorded.
uint32_t TLASManager::draw(BLASHandle blas_handle, uint32_t material_id) {
    PROFILE_SECTION("TLAS Draw Call");
    
    if (blas_handle == INVALID_BLAS_HANDLE) return 0;
    
    if (draw_records_.size() >= static_cast<size_t>(max_instances_)) {
        printf("Warning: TLAS manager draw capacity exceeded (%d)\n", max_instances_);
        return 0;
    }
    
    uint32_t instance_id = next_instance_id_++;
    draw_records_.emplace_back(blas_handle, get_current_matrix(), material_id, instance_id);
    
    mark_dirty();
    return instance_id;
}

// Record a whole vector of instances. Each entry's `transform` REPLACES the
// current matrix rather than composing with it (push / load / draw / pop), so
// batch transforms are absolute, unlike the incremental `translate`/`rotate`
// helpers. `is_imposter` is copied onto the record only when the draw actually
// succeeded; refused entries are skipped entirely.
void TLASManager::draw_batch(const std::vector<DrawInstance>& instances) {
    PROFILE_SECTION("TLAS Batch Draw");

    for (const auto& instance : instances) {
        push_matrix();
        load_matrix(instance.transform);
        if (draw(instance.blas_handle, instance.material_id) != 0)
            draw_records_.back().is_imposter = instance.is_imposter; // draw appended a record
        pop_matrix();
    }
}

void TLASManager::ensure_instance_capacity(int n) {
    if (n <= max_instances_) return;
    max_instances_ = n;
    draw_records_.reserve(static_cast<size_t>(n));
}

// Drop every recorded instance and the built TLAS, and reset the matrix stack
// to a single identity. Instance ids restart at 1, so ids are REUSED across
// clears — anything caching an id across a clear must revalidate it. Buffer
// capacity is retained (`vector::clear`), which is the point: clear/redraw/build
// each frame does not re-allocate. Bumps `content_revision()`.
void TLASManager::clear() {
    draw_records_.clear();
    next_instance_id_ = 1;
    
    // Reset matrix stack to just identity. `suppressed_pushes_` goes with it:
    // it only makes sense paired with the stack it was counted against, and
    // leaving it set would make the next pop_matrix() a no-op.
    while (matrix_stack_.size() > 1) {
        matrix_stack_.pop();
    }
    suppressed_pushes_ = 0;
    load_identity();
    
    // Clean up existing TLAS and instances
    tlas_.reset(nullptr);
    instances_.clear();
    
    mark_dirty();
}

// Flatten the recorded instances into a freshly built TLAS. A FULL rebuild
// every call — there is no incremental update path — so cost is O(draw records)
// plus the TLAS build itself, and it allocates a `BVHInstance` per record.
//
// Preconditions and sharp edges:
//  - Records whose BLAS handle is missing from `blas_manager` are SKIPPED with
//    a printf, so the built instance count can be lower than the record count.
//  - With no draw records it warns and returns EARLY, leaving the previously
//    built TLAS in place. To end up with no TLAS, call `clear()`.
//  - `tlas_` is reset before `instance_storage_` is touched because the TLAS
//    holds a raw pointer into that vector.
//  - `record.transform` (mm::Mat4) and `mat4` are the same row-major float[16],
//    hence the plain element copy below.
void TLASManager::build(const BLASManager& blas_manager) {
    PROFILE_SECTION("TLAS Build");
    
    if (draw_records_.empty()) {
        printf("Warning: No draw records to build TLAS from\n");
        return;
    }
    
    // Clean up existing TLAS and instances
    tlas_.reset(nullptr);
    instances_.clear();
    
    // Create BVH instances from draw records (using unique_ptr approach for now)
    instances_.reserve(draw_records_.size());
    std::vector<BVHInstance*> instance_ptrs;
    instance_ptrs.reserve(draw_records_.size());

    for (const auto& record : draw_records_) {
        // Get BVH from manager
        BVH* bvh = blas_manager.get_bvh(record.blas_handle);
        if (!bvh) {
            printf("Warning: BLAS handle %u not found in BLAS manager\n", record.blas_handle);
            continue; // skip: no instance/BVH for this record
        }

        // Create BVH instance
        auto instance = std::make_unique<BVHInstance>(bvh, record.instance_id);

        // Convert and set transform - this will also calculate world bounds.
        // mm::Mat4 and mat4 (SpatialQueryLib/tri.h) are both row-major
        // float[16] with translation at [3],[7],[11] -- same layout, plain
        // element copy (the convert_matrix() shim this replaced was doing
        // exactly this loop).
        mat4 new_transform;
        for (int i = 0; i < 16; i++) {
            new_transform.cell[i] = record.transform.m[i];
        }
        instance->SetTransform(new_transform);

        // Add to our vectors — record and instance are added in lock-step
        instance_ptrs.push_back(instance.get());
        instances_.push_back(std::move(instance));
    }
    
    // Create and build TLAS
    if (!instance_ptrs.empty()) {
        tlas_.reset(); // old TLAS points into instance_storage_; drop it before mutating
        instance_storage_.clear();
        instance_storage_.reserve(instance_ptrs.size());
        for (BVHInstance* p : instance_ptrs) {
            instance_storage_.push_back(*p);
        }
        // TLAS's constructor already calls Build(); a second explicit Build()
        // here just rebuilt the whole top level a second time, every rebuild.
        tlas_ = std::make_unique<TLAS>(instance_storage_.data(), static_cast<int>(instance_storage_.size()));
    }

    mark_dirty();
}

// Instances in the BUILT TLAS, not recorded draws: 0 before the first `build()`,
// and lower than `get_draw_record_count()` when records were skipped for a
// missing BLAS. Use `get_draw_record_count()` for what was recorded.
int TLASManager::get_instance_count() const {
    return tlas_ ? tlas_->blasCount : 0;
}

int TLASManager::get_node_count() const {
    return tlas_ ? tlas_->nodesUsed : 0;
}

// Scene building utilities implementation
// Convenience scene generators: each records `count` (or rows*cols) instances of
// a single BLAS through the normal `draw()` path, so all of the manager's rules
// apply — the caller must still call `build()`, and the instance ceiling still
// silently caps the output.
//
// `TLAS_PUSH_MATRIX(manager)` declares a `ScopedMatrix` whose scope is the loop
// BODY, so each iteration's transform is popped on the way out and the
// transforms below do not accumulate.
//
// `create_scatter` draws from the global `std::rand()` and never seeds it, so
// its layout depends on process-wide RNG state — not reproducible, and not
// suitable for anything cached by content hash.
namespace SceneBuilder {

void create_grid(TLASManager& manager, BLASHandle blas_handle, 
                int rows, int cols, float spacing, uint32_t material_id) {
    PROFILE_SECTION("Create Grid Scene");
    
    float start_x = -(cols - 1) * spacing * 0.5f;
    float start_z = -(rows - 1) * spacing * 0.5f;
    
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            TLAS_PUSH_MATRIX(manager);
            
            float x = start_x + col * spacing;
            float z = start_z + row * spacing;
            
            manager.translate(x, 0.0f, z);
            manager.draw(blas_handle, material_id);
        }
    }
}

void create_circle(TLASManager& manager, BLASHandle blas_handle,
                  int count, float radius, uint32_t material_id) {
    PROFILE_SECTION("Create Circle Scene");
    
    for (int i = 0; i < count; i++) {
        TLAS_PUSH_MATRIX(manager);
        
        float angle = static_cast<float>(i) / static_cast<float>(count) * 2.0f * static_cast<float>(M_PI);
        float x = radius * std::cos(angle);
        float z = radius * std::sin(angle);
        
        manager.translate(x, 0.0f, z);
        manager.rotate_y(angle); // Face inward
        manager.draw(blas_handle, material_id);
    }
}

void create_scatter(TLASManager& manager, BLASHandle blas_handle,
                   int count, float range, uint32_t material_id) {
    PROFILE_SECTION("Create Scatter Scene");
    
    for (int i = 0; i < count; i++) {
        TLAS_PUSH_MATRIX(manager);
        
        // Random position
        float x = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * range * 2.0f;
        float y = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * range * 0.5f;
        float z = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * range * 2.0f;
        
        // Random rotation
        float rot_y = static_cast<float>(std::rand()) / RAND_MAX * 2.0f * static_cast<float>(M_PI);
        
        // Random scale
        float scale_factor = 0.5f + (static_cast<float>(std::rand()) / RAND_MAX) * 1.0f;
        
        manager.translate(x, y, z);
        manager.rotate_y(rot_y);
        manager.scale(scale_factor);
        manager.draw(blas_handle, material_id);
    }
}

} // namespace SceneBuilder