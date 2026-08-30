#pragma once

#include "matter/character.h"
#include "matter/scene.h"
#include "matter/world_session.h"

#include <string>

namespace matter::scene { class SimulationControl; }

namespace viewer {

struct CharacterWalkInput {
    matter::Float3 world_direction{};
    bool sprint = false;
    bool jump_pressed = false;
};

class CharacterJumpEdge {
public:
    bool update(bool space_down, bool accepts_keyboard) noexcept;
    void reset() noexcept;
private:
    bool armed_ = false;
};

class CharacterWalkController {
public:
    bool set_enabled(flecs::world&, matter::scene::SimulationControl&,
                     bool enabled, bool session_ready, std::string& error);
    bool set_intent(flecs::world&, matter::Float3 direction, bool sprint, std::string& error);
    void clear_intent(flecs::world&);
    bool latch_jump(flecs::world&, matter::scene::SimulationMode, std::string& error);
    void sample(flecs::world&, matter::scene::SimulationMode, const CharacterWalkInput&);
    void reset(flecs::world&);
    bool enabled() const noexcept;
    bool eye_position(flecs::world&, matter::Float3& out) const;
    bool status_json(flecs::world&, matter::scene::SimulationMode,
                     const std::string& label, std::string& out, std::string& error) const;
private:
    flecs::entity resolve(flecs::world&, std::string& error) const;
    bool enabled_ = false;
    bool override_ = false;
    CharacterWalkInput intent_{};
    matter::scene::SceneEntityId binding_{};
};

matter::TickDesc make_editor_tick(matter::scene::SimulationControl&,
                                 float wall_dt, float time_scale);
bool store_character_component(flecs::entity,
                                const matter::character::CharacterController&);

} // namespace viewer
