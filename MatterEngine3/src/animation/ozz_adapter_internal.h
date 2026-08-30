#pragma once

// MatterEngine3/src/animation/ozz_adapter_internal.h
//
// The pimpl bodies behind `ozz_adapter.h`.  Include this ONLY from the two
// adapter translation units (`ozz_adapter.cpp`, `ozz_bake.cpp`) and from
// adapter tests: it pulls ozz runtime headers in, and the entire point of
// `ozz_adapter.h` is that no other consumer has to.
//
// Both `Impl`s are plain state holders with no invariants of their own -- the
// free functions in the two adapter .cpp files are what keep `runtime` and the
// Matter-side metadata in agreement, and `deserialize_skeleton()` re-verifies
// that agreement whenever the state arrives from a blob.

#include "animation/ozz_adapter.h"

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/memory/unique_ptr.h"

namespace matter::animation {

// Private runtime state shared by the runtime adapter and the bake-only Ozz
// compiler.  Keeping this out of the public adapter header prevents offline
// Ozz headers from leaking into runtime consumers.
struct OzzSkeleton::Impl {
    ozz::unique_ptr<ozz::animation::Skeleton> runtime;  // null until built or deserialized
    std::vector<JointIndex> parents;   // canonical index -> parent index; kInvalidJoint at the root
    std::vector<JointRange> subtrees;  // half-open [begin, end) descendant range, begin == own index
    std::vector<AnimationTransform> rest_locals;  // rest/bind locals, one per joint, canonical order
};

// `tracks` mirrors `runtime->num_tracks()`.  It is duplicated because it is
// also written into the `MOA1` archive header, where it exists purely so the
// loader can catch a header that disagrees with the payload.
struct OzzAnimation::Impl {
    ozz::unique_ptr<ozz::animation::Animation> runtime;
    int tracks = 0;
};

} // namespace matter::animation
