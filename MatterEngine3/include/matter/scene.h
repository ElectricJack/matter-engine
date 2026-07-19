#pragma once

#include <cstdint>

namespace matter::scene {

struct SceneEntityId { uint64_t value = 0; };
struct PartInstance { uint64_t part_hash = 0; bool visible = true; bool casts_shadow = true; };
enum class PartInstanceErrorCode : uint8_t { None, MissingPart, PartUnavailable, RendererCapacity };
struct PartInstanceError { PartInstanceErrorCode code = PartInstanceErrorCode::None; uint64_t part_hash = 0; };

} // namespace matter::scene
