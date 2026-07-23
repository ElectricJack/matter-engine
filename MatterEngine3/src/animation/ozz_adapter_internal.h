#pragma once

#include "animation/ozz_adapter.h"

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/memory/unique_ptr.h"

namespace matter::animation {

// Private runtime state shared by the runtime adapter and the bake-only Ozz
// compiler.  Keeping this out of the public adapter header prevents offline
// Ozz headers from leaking into runtime consumers.
struct OzzSkeleton::Impl {
    ozz::unique_ptr<ozz::animation::Skeleton> runtime;
    std::vector<JointIndex> parents;
    std::vector<JointRange> subtrees;
    std::vector<AnimationTransform> rest_locals;
};

struct OzzAnimation::Impl {
    ozz::unique_ptr<ozz::animation::Animation> runtime;
    int tracks = 0;
};

} // namespace matter::animation
