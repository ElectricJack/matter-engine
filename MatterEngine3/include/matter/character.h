#pragma once

#include <cstdint>

#include "flecs.h"
#include "matter/math_types.h"

namespace matter::character {

struct CharacterController {
    float radius = 0.4f;
    float height = 1.8f;
    float move_speed = 4.5f;
    float max_slope_cos = 0.70710678f;
    float step_up_height = 0.45f;
    float jump_speed = 5.0f;
    Float3 velocity{};
    bool grounded = false;
    uint64_t fixed_ticks = 0;
    uint32_t jumps_consumed = 0;
    uint32_t jumps_started = 0;
};

struct MoveIntent {
    Float3 move_dir{};
    bool jump = false;
    bool sprint = false;
};

bool valid_character_configuration(const CharacterController&) noexcept;
void register_character_systems(flecs::world&);
struct CharacterModule {
    explicit CharacterModule(flecs::world&);
};

} // namespace matter::character
