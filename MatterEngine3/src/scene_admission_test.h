#pragma once

#include <functional>
#include <string>

namespace matter::detail {
// Internal, scoped native fixture seam. The real session worker, publication
// jobs, streaming, ECS and collision transaction execute normally. Only the
// device's visual-admission decision is supplied by the fixture. This cannot
// establish Vulkan recording/submission or editor UI acceptance.
struct SceneAdmissionTest {
    // Called on the app thread after authored finalize. 0: pending, 1: visual
    // preparation succeeded, -1: rejected (fill error with the admission tag).
    std::function<int(std::string&)> visual;
    bool finalized = false;
    bool fill_ready = false;
};
inline thread_local SceneAdmissionTest* scene_admission_test = nullptr;
class ScopedSceneAdmissionTest {
    SceneAdmissionTest* previous_;
public:
    explicit ScopedSceneAdmissionTest(SceneAdmissionTest& test)
        : previous_(scene_admission_test) { scene_admission_test = &test; }
    ~ScopedSceneAdmissionTest() { scene_admission_test = previous_; }
};
} // namespace matter::detail
