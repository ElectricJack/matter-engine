#include "check.h"
#include "../src/hydrology/hydrology_settings.h"

#include <limits>
#include <string>

namespace {

matter::HydrologyWorldSettings valid_river_settings() {
    matter::HydrologyWorldSettings settings{};
    settings.enabled = true;
    settings.domain.origin_m = {-8.0f, -2.0f, -16.0f};
    settings.domain.nx = 96;
    settings.domain.ny = 20;
    settings.domain.nz = 48;
    settings.domain.cell_size_m = 0.5f;
    settings.dt_s = 0.005f;
    settings.gravity_mps2 = 9.81f;
    settings.downstream_xz = {1.0f, 0.0f};
    settings.residual_head_gradient_xz = {-0.01f, 0.0f};
    settings.inlet_flow_m3s = 1.0f;
    settings.inlet_head_m = 4.0f;
    settings.outlet_head_m = 1.5f;
    settings.batch_steps = 256;
    settings.max_steps = 16384;
    return settings;
}

bool rejects(const matter::HydrologyWorldSettings& settings) {
    hydrology::HydrologyBakeDescription description{};
    std::string error;
    return !hydrology::validate_and_key(settings, 41, description, error) &&
           !error.empty();
}

void test_hydrology_settings_accept_the_fixed_river_domain() {
    hydrology::HydrologyBakeDescription description{};
    std::string error;
    CHECK(hydrology::validate_and_key(valid_river_settings(), 41, description, error),
          error.c_str());
    CHECK(description.domain.nx == 96 && description.domain.ny == 20 &&
              description.domain.nz == 48,
          "the canonical description retains the authored fixed domain");
    CHECK(description.terrain_revision == 41,
          "the canonical description retains the terrain revision");
}

void test_hydrology_settings_reject_invalid_fixed_domain_values() {
    auto settings = valid_river_settings();
    settings.gravity_mps2 = std::numeric_limits<float>::infinity();
    CHECK(rejects(settings), "nonfinite hydrology values are rejected");

    settings = valid_river_settings();
    settings.domain.nx = 0;
    CHECK(rejects(settings), "zero dimensions are rejected");

    settings = valid_river_settings();
    settings.domain.nx = 4097;
    CHECK(rejects(settings), "oversized dimensions are rejected");

    settings = valid_river_settings();
    settings.domain.cell_size_m = 0.0f;
    CHECK(rejects(settings), "zero cell sizes are rejected");

    settings = valid_river_settings();
    settings.dt_s = 0.0f;
    CHECK(rejects(settings), "zero time steps are rejected");

    settings = valid_river_settings();
    settings.max_steps = 0;
    CHECK(rejects(settings), "zero horizons are rejected");

    settings = valid_river_settings();
    settings.batch_steps = 255;
    CHECK(rejects(settings), "horizons must divide evenly into batches");

    settings = valid_river_settings();
    settings.residual_head_gradient_xz = {0.01f, 0.0f};
    CHECK(rejects(settings), "the residual grade must descend along downstream");
}

void test_hydrology_settings_key_changes_with_terrain_revision() {
    const matter::HydrologyWorldSettings settings = valid_river_settings();
    hydrology::HydrologyBakeDescription a{}, b{};
    std::string error;
    CHECK(hydrology::validate_and_key(settings, 41, a, error), error.c_str());
    const std::uint64_t one_bit_changed_revision = 41ull ^ (1ull << 5);
    CHECK(hydrology::validate_and_key(settings, one_bit_changed_revision, b, error),
          error.c_str());
    CHECK(a.semantic_key != b.semantic_key,
          "one terrain-revision bit changes the semantic key");
}

} // namespace

int main() {
    test_hydrology_settings_accept_the_fixed_river_domain();
    test_hydrology_settings_reject_invalid_fixed_domain_values();
    test_hydrology_settings_key_changes_with_terrain_revision();
    return check_summary();
}
