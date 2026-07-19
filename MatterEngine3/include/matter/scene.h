#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace matter::scene {

struct SceneEntityId { uint64_t value = 0; };
struct PartInstance { uint64_t part_hash = 0; bool visible = true; bool casts_shadow = true; };
enum class PartInstanceErrorCode : uint8_t { None, MissingPart, PartUnavailable, RendererCapacity };
struct PartInstanceError { PartInstanceErrorCode code = PartInstanceErrorCode::None; uint64_t part_hash = 0; };

// Engine-side scene record: copied data safe to hold across frames.
// Internal entities without SceneEntityId are never exposed.
struct SceneRecord {
    SceneEntityId id{};
    SceneEntityId parent_id{};  // zero if root
    std::string name;
    std::vector<std::string> component_names;
};

enum class SceneEditError : uint8_t {
    None,
    EntityNotFound,
    CycleDetected,
    InvalidTarget
};

struct SceneEditResult {
    SceneEditError error = SceneEditError::None;
    SceneEntityId created_id{};  // set on create/duplicate
};

} // namespace matter::scene
