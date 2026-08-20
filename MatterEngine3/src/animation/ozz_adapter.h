#pragma once

// MatterEngine3/src/animation/ozz_adapter.h
//
// The animation subsystem's entire surface onto ozz-animation.  No ozz header
// appears here: `OzzSkeleton`, `OzzAnimation` and `OzzSampleContext` are pimpl
// handles whose `Impl` lives in `ozz_adapter_internal.h`, so the rest of
// MatterEngine3 compiles without ozz on its include path.
//
// The implementation is split across two translation units:
//   - `ozz_adapter.cpp` -- runtime: (de)serialize, sample, blend,
//     local_to_model, solve_two_bone.
//   - `ozz_bake.cpp`    -- offline: build_skeleton / build_clip from the
//     authored `RigDefinition` / `ClipDefinition`.  Only that file may include
//     ozz's `animation/offline/*` headers.
//
// Conventions
//   - Joints are addressed by `JointIndex` in Matter's canonical depth-first
//     order (defined in `animation_validate.cpp` / `ozz_bake.cpp`).
//     `kInvalidJoint` is the null index and `JointRange` is half-open
//     [begin, end).
//   - Local transforms are parent-relative `AnimationTransform`s; model
//     matrices are `Mat4f`, rows stored contiguously.
//   - Every function returns bool: false means rejected input or a failed ozz
//     job, never an exception.  The two `deserialize_*` entry points
//     additionally append `ozz-archive` diagnostics.
//   - The handle types are move-only, own heap state, and carry no internal
//     synchronization: one `OzzSampleContext` backs one in-flight `sample()`.
//   - Ownership note: the `friend` list on each handle is exactly the set of
//     free functions allowed to reach its `Impl`; adding an operation means
//     adding a friend, which is the intended friction.

#include "animation/animation_ir.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace matter::animation {

// Ozz implementation objects are intentionally hidden. Every caller crosses
// this boundary using only Matter IR, transforms, matrices, and byte buffers.
// A baked rig: ozz's runtime `Skeleton` plus the Matter-side metadata that
// travels with it (canonical parent indices, half-open subtree ranges, and the
// authored rest locals).  Produced once by `build_skeleton()` at bake time or
// `deserialize_skeleton()` at load, then read-only for the rest of its life;
// the runtime never mutates one.  Move-only, and a default-constructed
// instance is empty (`joint_count() == 0`) rather than invalid.
class OzzSkeleton {
public:
    OzzSkeleton();
    ~OzzSkeleton();
    OzzSkeleton(OzzSkeleton&&) noexcept;
    OzzSkeleton& operator=(OzzSkeleton&&) noexcept;
    OzzSkeleton(const OzzSkeleton&) = delete;
    OzzSkeleton& operator=(const OzzSkeleton&) = delete;

    // Out-of-range joints are answered, not asserted: `parent()` returns
    // `kInvalidJoint` and `subtree()` returns a default `JointRange` --
    // and note that default range is the same {kInvalidJoint, kInvalidJoint}
    // value `local_to_model()` reads as "the whole skeleton", so a bad index
    // fed straight through widens the operation instead of failing it.
    std::size_t joint_count() const;
    JointIndex parent(JointIndex joint) const;
    JointRange subtree(JointIndex joint) const;
    // Copies the authored skeleton-local rest transform.  Runtime users need
    // this for operations such as root locking that must preserve an authored
    // root offset without exposing Ozz implementation data.
    bool rest_local(JointIndex joint, AnimationTransform& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool build_skeleton(const RigDefinition&, OzzSkeleton&, Diagnostics&);
    friend bool serialize_skeleton(const OzzSkeleton&, std::vector<uint8_t>&);
    friend bool deserialize_skeleton(const uint8_t*, std::size_t, OzzSkeleton&, Diagnostics&);
    friend bool blend(const OzzSkeleton&, const std::vector<struct BlendLayer>&, const std::vector<struct AdditiveLayer>&, std::vector<AnimationTransform>&);
    friend bool local_to_model(const OzzSkeleton&, const std::vector<AnimationTransform>&, std::vector<Mat4f>&, JointRange);
    friend bool solve_two_bone(const struct TwoBoneSolve&, const std::vector<Mat4f>&, std::vector<AnimationTransform>&, std::vector<Mat4f>&);
};

// A baked clip: ozz's runtime `Animation` -- already key-reduced by the
// offline optimizer in `ozz_bake.cpp`, so its keys are not the authored keys --
// plus the track count carried in the archive header.  `duration()` is in
// seconds and `name()` is the authored clip name; both return an empty/zero
// value on a handle that was never built or deserialized.  Move-only.
class OzzAnimation {
public:
    OzzAnimation();
    ~OzzAnimation();
    OzzAnimation(OzzAnimation&&) noexcept;
    OzzAnimation& operator=(OzzAnimation&&) noexcept;
    OzzAnimation(const OzzAnimation&) = delete;
    OzzAnimation& operator=(const OzzAnimation&) = delete;
    std::size_t track_count() const;
    float duration() const;
    std::string name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool build_clip(const RigDefinition&, const ClipDefinition&, OzzAnimation&, Diagnostics&);
    friend bool serialize_animation(const OzzAnimation&, std::vector<uint8_t>&);
    friend bool deserialize_animation(const uint8_t*, std::size_t, OzzAnimation&, Diagnostics&);
    friend bool sample(const OzzAnimation&, float, struct OzzSampleContext&, std::vector<AnimationTransform>&);
};

// Per-sampler scratch state that ozz needs to sample a clip efficiently: its
// interpolation cache plus the SoA output buffer, both owned for the lifetime
// of the context.  Keep one per concurrently sampled clip instance -- sharing
// one across two clips is correct but throws the cache away every call.
//
// It grows on demand inside `sample()` (an animation with more tracks than the
// context was built for reallocates it) and never shrinks, which is why the
// animation budget accounts for it up front through
// `mutable_bytes_for_tracks()` (see `animation/animation_store.cpp`).
struct OzzSampleContext {
    OzzSampleContext();
    ~OzzSampleContext();
    OzzSampleContext(OzzSampleContext&&) noexcept;
    OzzSampleContext& operator=(OzzSampleContext&&) noexcept;
    OzzSampleContext(const OzzSampleContext&) = delete;
    OzzSampleContext& operator=(const OzzSampleContext&) = delete;

    // Exact persistent allocation made by sample() for one context with this
    // many animation tracks, including adapter and Ozz-owned storage.
    static std::size_t mutable_bytes_for_tracks(std::size_t track_count) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool sample(const OzzAnimation&, float, OzzSampleContext&, std::vector<AnimationTransform>&);
};

// Inputs to `blend()`.  `locals` is a NON-OWNING pointer to a pose that must
// stay alive for the duration of the call; null means "layer not present" and
// is skipped.  Every non-null pose must have exactly one entry per skeleton
// joint.  `weight` is a normalized [0,1] contribution and is not renormalized
// across layers -- what the weights do not account for falls back to the
// skeleton's rest pose.
//
// An `AdditiveLayer` pose holds bind-relative DELTAS, not absolute local
// transforms; those come from an additive clip, converted once at bake time by
// `bind_relative_delta()` in `ozz_bake.cpp`.
struct BlendLayer { const std::vector<AnimationTransform>* locals = nullptr; float weight = 0.0f; };
struct AdditiveLayer { const std::vector<AnimationTransform>* locals = nullptr; float weight = 0.0f; };

// One two-bone IK request for `solve_two_bone()`.  `start`/`mid`/`end` must be
// a straight grandparent-parent-child chain in `skeleton` (checked at the call
// site), and `affected` must be exactly `skeleton->subtree(start)`.
//
//  - `target` and `pole_vector` are MODEL space.  `animation_targets.cpp`
//    stores the authored pole animator-root-relative and rotates it into model
//    space by the chain root's model rotation before filling this in.
//  - `mid_axis` is the hinge axis whose sign tells ozz which way the chain is
//    already bent.  It comes from `CanonicalTarget::bend_axis`, derived at
//    canonicalization time -- the cross-product order there is load-bearing,
//    see the long comment in `animation_validate.cpp` about the chain
//    "correcting" through the straight position.
//  - `twist_angle` rotates the chain about the start-to-end axis; `soften`
//    and `weight` are normalized [0,1], `weight` being the IK blend factor.
//  - The defaults are the neutral ones (+Y pole, +Z hinge, full weight); a
//    real target overwrites all of them.
struct TwoBoneSolve {
    const OzzSkeleton* skeleton = nullptr;
    JointIndex start = kInvalidJoint;
    JointIndex mid = kInvalidJoint;
    JointIndex end = kInvalidJoint;
    Float3 target{};
    Float3 pole_vector{0.0f, 1.0f, 0.0f};
    Float3 mid_axis{0.0f, 0.0f, 1.0f};
    float twist_angle = 0.0f;
    float soften = 1.0f;
    float weight = 1.0f;
    JointRange affected{};
};

// The free-function API.  `build_*` are BAKE-ONLY (implemented in
// `ozz_bake.cpp`, which pulls in ozz's offline headers); everything below them
// is the runtime path in `ozz_adapter.cpp`.  Preconditions, sentinel values
// and failure meanings are documented at each definition rather than repeated
// here -- in particular `local_to_model`'s `affected` range rules and
// `sample`'s normalized ratio.
bool build_skeleton(const RigDefinition&, OzzSkeleton&, Diagnostics&);
bool build_clip(const RigDefinition&, const ClipDefinition&, OzzAnimation&, Diagnostics&);
bool serialize_skeleton(const OzzSkeleton&, std::vector<uint8_t>&);
bool serialize_animation(const OzzAnimation&, std::vector<uint8_t>&);
bool deserialize_skeleton(const uint8_t* data, std::size_t size, OzzSkeleton&, Diagnostics&);
bool deserialize_animation(const uint8_t* data, std::size_t size, OzzAnimation&, Diagnostics&);
bool sample(const OzzAnimation&, float ratio, OzzSampleContext&, std::vector<AnimationTransform>& locals);
bool blend(const OzzSkeleton&, const std::vector<BlendLayer>&, const std::vector<AdditiveLayer>&, std::vector<AnimationTransform>& locals);
bool local_to_model(const OzzSkeleton&, const std::vector<AnimationTransform>& locals,
                    std::vector<Mat4f>& models, JointRange affected = {});
bool solve_two_bone(const TwoBoneSolve&, const std::vector<Mat4f>& models,
                    std::vector<AnimationTransform>& locals, std::vector<Mat4f>& updated_models);

} // namespace matter::animation
