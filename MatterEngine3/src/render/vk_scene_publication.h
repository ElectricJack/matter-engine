#pragma once
#include "vk_scene_renderer.h"

namespace viewer {
// Internal owner-lane transaction. Ordinary registrations are additive until
// commit; existing geometry is never released to change debug LOD thresholds.
// No GPU command may be recorded while this guard is preparing a candidate.
class ScenePublication {
public:
    explicit ScenePublication(VkSceneRenderer&);
    ~ScenePublication();
    ScenePublication(const ScenePublication&)=delete;
    ScenePublication& operator=(const ScenePublication&)=delete;
    void added_part(uint64_t hash);
    bool prepare_lods(const VkScenePart&,std::string& error);
    bool prepare_instances(const std::vector<VkSceneInstance>&,std::string& error);
    // Call only after the primary/shadow setter succeeded. This phase performs
    // no allocation, uploads, resource destruction or fallible preparation.
    void commit() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
