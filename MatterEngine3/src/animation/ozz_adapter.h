#pragma once

#include "animation/animation_ir.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace matter::animation {

// Ozz implementation objects are intentionally hidden. Every caller crosses
// this boundary using only Matter IR, transforms, matrices, and byte buffers.
class OzzSkeleton {
public:
    OzzSkeleton();
    ~OzzSkeleton();
    OzzSkeleton(OzzSkeleton&&) noexcept;
    OzzSkeleton& operator=(OzzSkeleton&&) noexcept;
    OzzSkeleton(const OzzSkeleton&) = delete;
    OzzSkeleton& operator=(const OzzSkeleton&) = delete;

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

struct OzzSampleContext {
    OzzSampleContext();
    ~OzzSampleContext();
    OzzSampleContext(OzzSampleContext&&) noexcept;
    OzzSampleContext& operator=(OzzSampleContext&&) noexcept;
    OzzSampleContext(const OzzSampleContext&) = delete;
    OzzSampleContext& operator=(const OzzSampleContext&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool sample(const OzzAnimation&, float, OzzSampleContext&, std::vector<AnimationTransform>&);
};

struct BlendLayer { const std::vector<AnimationTransform>* locals = nullptr; float weight = 0.0f; };
struct AdditiveLayer { const std::vector<AnimationTransform>* locals = nullptr; float weight = 0.0f; };

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
