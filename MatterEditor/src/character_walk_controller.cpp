#include "character_walk_controller.h"
#include "viewer_commands.h"
#include "ecs/scene_registry.h"
#include "ecs/simulation_control.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace viewer {
namespace {

bool finite(matter::Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

matter::Float3 horizontal(matter::Float3 value) {
    // Double intermediates keep even finite FLT_MAX inputs normalizable.
    const double length = std::hypot(static_cast<double>(value.x), static_cast<double>(value.z));
    if (length > 1.0) return {static_cast<float>(value.x / length), 0, static_cast<float>(value.z / length)};
    return {value.x, 0, value.z};
}

const char* mode_name(matter::scene::SimulationMode mode) {
    using matter::scene::SimulationMode;
    switch (mode) {
        case SimulationMode::Edit: return "edit";
        case SimulationMode::Play: return "play";
        case SimulationMode::Pause: return "pause";
    }
    return "unknown";
}

} // namespace

bool CharacterJumpEdge::update(bool space_down, bool accepts_keyboard) noexcept {
    if (!accepts_keyboard) { armed_ = false; return false; }
    if (!space_down) { armed_ = true; return false; }
    const bool pressed = armed_;
    armed_ = false;
    return pressed;
}
void CharacterJumpEdge::reset() noexcept { armed_ = false; }

flecs::entity CharacterWalkController::resolve(flecs::world& world, std::string& error) const {
    const uint64_t authored = matter::scene::hash_authored_id("river-player");
    flecs::entity found{};
    unsigned count = 0;
    world.each([&](flecs::entity entity, const matter::scene::SceneEntityId& id) {
        if (id.value == authored) { found = entity; ++count; }
    });
    if (count != 1) {
        error = count == 0 ? "river-player is missing" : "river-player identity is ambiguous";
        return {};
    }
    const auto id = found.get<matter::scene::SceneEntityId>();
    if (id.generation == 0 || (enabled_ &&
        (id.value != binding_.value || id.generation != binding_.generation))) {
        error = "river-player identity generation is stale or invalid";
        return {};
    }
    const auto* controller = found.try_get<matter::character::CharacterController>();
    if (!controller || !found.has<matter::character::MoveIntent>()) {
        error = "river-player requires CharacterController and MoveIntent";
        return {};
    }
    if (!matter::scene::validate_character_component(found, *controller, error)) return {};
    error.clear();
    return found;
}

bool CharacterWalkController::set_enabled(flecs::world& world, matter::scene::SimulationControl& control,
                                         bool enabled, bool session_ready, std::string& error) {
    if (!enabled) { reset(world); error.clear(); return true; }
    if (!session_ready) { error = "walking requires a Ready session with configured collision installed"; return false; }
    const auto entity = resolve(world, error);
    if (!entity.is_valid()) return false;
    if (control.mode() != matter::scene::SimulationMode::Play && !control.play(world, error)) return false;
    binding_ = entity.get<matter::scene::SceneEntityId>();
    enabled_ = true;
    error.clear();
    return true;
}

bool CharacterWalkController::set_intent(flecs::world& world, matter::Float3 direction,
                                         bool sprint, std::string& error) {
    if (!enabled_) { error = "character intent requires walking enabled"; return false; }
    if (!resolve(world, error).is_valid()) return false;
    if (!finite(direction)) { error = "character intent requires finite direction"; return false; }
    intent_ = {horizontal(direction), sprint, false};
    override_ = true;
    error.clear();
    return true;
}

void CharacterWalkController::clear_intent(flecs::world& world) {
    override_ = false;
    intent_ = {};
    // Fail-closed cleanup also covers a replaced/ambiguous identity. Only the
    // authored player(s) are touched; never retain or reuse an entity handle.
    const uint64_t authored = matter::scene::hash_authored_id("river-player");
    world.each([&](const matter::scene::SceneEntityId& id, matter::character::MoveIntent& intent) {
        if (id.value == authored) intent = {};
    });
}

bool CharacterWalkController::latch_jump(flecs::world& world, matter::scene::SimulationMode mode,
                                        std::string& error) {
    if (!enabled_ || (mode != matter::scene::SimulationMode::Play && mode != matter::scene::SimulationMode::Pause)) {
        error = "character jump requires walking in Play or Pause"; return false;
    }
    const auto entity = resolve(world, error);
    if (!entity.is_valid()) return false;
    auto intent = entity.get<matter::character::MoveIntent>();
    intent.jump = true;
    entity.set<matter::character::MoveIntent>(intent);
    error.clear();
    return true;
}

void CharacterWalkController::sample(flecs::world& world, matter::scene::SimulationMode mode,
                                     const CharacterWalkInput& live) {
    if (!enabled_) return;
    std::string error;
    const auto entity = resolve(world, error);
    if (!entity.is_valid()) { reset(world); return; }
    if (mode != matter::scene::SimulationMode::Play && mode != matter::scene::SimulationMode::Pause) {
        entity.set<matter::character::MoveIntent>({});
        return;
    }
    const CharacterWalkInput input = override_ ? intent_ : live;
    auto intent = entity.get<matter::character::MoveIntent>();
    if (finite(input.world_direction)) {
        intent.move_dir = horizontal(input.world_direction);
        intent.sprint = input.sprint;
    } else {
        intent.move_dir = {};
        intent.sprint = false;
    }
    // A no-edge render frame must never clear a paused/pending press. Live
    // Space remains independent of the directional automation override.
    intent.jump = intent.jump || (live.jump_pressed && finite(live.world_direction));
    entity.set<matter::character::MoveIntent>(intent);
}

void CharacterWalkController::reset(flecs::world& world) {
    clear_intent(world);
    binding_ = {};
    enabled_ = false;
}
bool CharacterWalkController::enabled() const noexcept { return enabled_; }

bool CharacterWalkController::eye_position(flecs::world& world, matter::Float3& out) const {
    if (!enabled_) return false;
    std::string error;
    const auto entity = resolve(world, error);
    if (!entity.is_valid()) return false;
    auto eye = entity.get<matter::ecs::LocalTransform>().translation;
    eye.y += entity.get<matter::character::CharacterController>().height * 0.5f - 0.1f;
    if (!finite(eye)) return false;
    out = eye;
    return true;
}

bool CharacterWalkController::status_json(flecs::world& world, matter::scene::SimulationMode mode,
                                          const std::string& label, std::string& out, std::string& error) const {
    if (!fifo_character_label_valid(label)) { error = "character status requires a safe 1-64 character label"; return false; }
    const auto entity = resolve(world, error);
    if (!entity.is_valid()) return false;
    const auto& controller = entity.get<matter::character::CharacterController>();
    const auto& intent = entity.get<matter::character::MoveIntent>();
    const auto& position = entity.get<matter::ecs::LocalTransform>().translation;
    if (!finite(intent.move_dir)) { error = "river-player has nonfinite intent"; return false; }
    const auto id = entity.get<matter::scene::SceneEntityId>();
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(std::numeric_limits<float>::max_digits10) << std::boolalpha;
    const auto vector = [&](matter::Float3 value) {
        json << '[' << value.x << ',' << value.y << ',' << value.z << ']';
    };
    json << "{\"label\":\"" << label << "\",\"authored_id\":\"river-player\",\"scene_id\":" << id.value
         << ",\"generation\":" << id.generation << ",\"mode\":\"" << mode_name(mode)
         << "\",\"walk_enabled\":" << enabled_ << ",\"position\":";
    vector(position);
    json << ",\"velocity\":"; vector(controller.velocity);
    json << ",\"grounded\":" << controller.grounded << ",\"fixed_ticks\":" << controller.fixed_ticks
         << ",\"jumps_consumed\":" << controller.jumps_consumed << ",\"jumps_started\":" << controller.jumps_started
         << ",\"jump_pending\":" << intent.jump << ",\"direction\":";
    vector(intent.move_dir);
    json << ",\"sprint\":" << intent.sprint << '}';
    out = json.str();
    error.clear();
    return true;
}

bool store_character_component(flecs::entity entity, const matter::character::CharacterController& value) {
    std::string error;
    if (!matter::scene::validate_character_component(entity, value, error)) return false;
    entity.set<matter::character::CharacterController>(value);
    return true;
}

// The editor and headless transport tests use this one tick-description seam.
matter::TickDesc make_editor_tick(matter::scene::SimulationControl& control,
                                 float wall_dt, float time_scale) {
    matter::TickDesc tick{};
    tick.presentation_delta_seconds = wall_dt;
    tick.frame_delta_seconds = wall_dt * time_scale;
    if (control.should_advance_fixed()) return tick;
    if (control.consume_pending_step()) {
        tick.frame_delta_seconds = tick.fixed_delta_seconds;
        tick.max_fixed_steps = 1;
        return tick;
    }
    tick.advance_fixed = false;
    tick.frame_delta_seconds = 0.0f;
    return tick;
}

} // namespace viewer
