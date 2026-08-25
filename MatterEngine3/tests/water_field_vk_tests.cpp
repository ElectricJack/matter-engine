#include "check.h"

#include "render/water_field_vk.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

viewer::PackedWaterField packed_one_cell(std::uint64_t runtime_digest,
                                         std::uint64_t presentation_digest) {
    hydrology::GameplayFieldLayout layout{};
    layout.origin_m = {10.0f, 2.0f, -4.0f};
    layout.cell_size_m = 0.5f;
    layout.width = 1;
    layout.depth = 1;
    std::vector<hydrology::GameplaySample> gameplay{{
        12.0f, 2.0f, 3.0f, 4.0f, 5.0f, true}};
    std::vector<hydrology::PresentationSample> presentation{{
        0.3f, 0.4f, 0.5f, 0.25f, 0.75f,
        hydrology::RiverFeature::Rapid, true}};
    viewer::PackedWaterField packed;
    viewer::WaterFieldError error;
    CHECK(viewer::pack_water_field(
              {layout, &gameplay, &presentation,
               runtime_digest, presentation_digest}, packed, error),
          error.message.c_str());
    return packed;
}

void test_packs_exact_three_image_contract() {
    hydrology::GameplayFieldLayout layout{};
    layout.origin_m = {10.0f, 2.0f, -4.0f};
    layout.cell_size_m = 0.5f;
    layout.width = 2;
    layout.depth = 2;
    std::vector<hydrology::GameplaySample> gameplay{
        {12.0f, 2.0f, 3.0f, 4.0f, 5.0f, true},
        {1.0e30f, 1.0f, -1.0e30f, 0.0f, 1.0f, true},
        {99.0f, 8.0f, 7.0f, 6.0f, 5.0f, false},
        {4.0f, 0.5f, 0.0f, 0.0f, 0.0f, true},
    };
    std::vector<hydrology::PresentationSample> presentation{
        {0.3f, 0.4f, 0.5f, 0.25f, 0.75f,
         hydrology::RiverFeature::Rapid, true},
        {0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
         hydrology::RiverFeature::Waterfall, true},
        {0.9f, 0.1f, 0.9f, 0.9f, 0.9f,
         hydrology::RiverFeature::Impact, false},
        {0.6f, 0.8f, 0.0f, 0.0f, 0.0f,
         hydrology::RiverFeature::Pool, true},
    };
    viewer::PackedWaterField packed;
    viewer::WaterFieldError error;
    CHECK(viewer::pack_water_field(
              {layout, &gameplay, &presentation, 0x11u, 0x22u},
              packed, error), error.message.c_str());
    CHECK(packed.image_a_rgba16f.size() == 16u &&
              packed.image_b_rgba16f.size() == 16u &&
              packed.image_c_rgba8.size() == 16u,
          "four cells produce three exact RGBA images");
    CHECK(viewer::water_half_to_float(packed.image_a_rgba16f[0]) == 12.0f &&
              viewer::water_half_to_float(packed.image_a_rgba16f[1]) == 2.0f &&
              viewer::water_half_to_float(packed.image_a_rgba16f[2]) == 3.0f &&
              viewer::water_half_to_float(packed.image_a_rgba16f[3]) == 5.0f,
          "image A packs surface height, depth, velocity X, velocity Z");
    CHECK(viewer::water_half_to_float(packed.image_b_rgba16f[0]) == 4.0f &&
              std::fabs(viewer::water_half_to_float(
                            packed.image_b_rgba16f[1]) - 0.3f) < 0.001f &&
              std::fabs(viewer::water_half_to_float(
                            packed.image_b_rgba16f[2]) - 0.4f) < 0.001f &&
              viewer::water_half_to_float(packed.image_b_rgba16f[3]) == 0.5f,
          "image B packs velocity Y, normal X/Z, and turbulence");
    const float packed_normal_x =
        viewer::water_half_to_float(packed.image_b_rgba16f[1]);
    const float packed_normal_z =
        viewer::water_half_to_float(packed.image_b_rgba16f[2]);
    const float reconstructed_y = std::sqrt(std::max(
        0.0f, 1.0f - packed_normal_x * packed_normal_x -
                          packed_normal_z * packed_normal_z));
    CHECK(std::fabs(reconstructed_y - 0.8660254f) < 0.001f,
          "packed normal X/Z are sufficient for deterministic positive-Y reconstruction");
    CHECK(packed.image_c_rgba8[0] == 64u &&
              packed.image_c_rgba8[1] == 191u &&
              packed.image_c_rgba8[2] == 255u &&
              viewer::decode_water_feature(packed.image_c_rgba8[3]) ==
                  hydrology::RiverFeature::Rapid,
          "image C packs aeration, foam, wet validity, and nearest feature id");
    CHECK(viewer::water_half_to_float(packed.image_a_rgba16f[4]) == 65504.0f &&
              viewer::water_half_to_float(packed.image_a_rgba16f[6]) == -65504.0f,
          "finite over-range values saturate to representable half-float bounds");
    for (std::size_t channel = 8u; channel != 12u; ++channel) {
        CHECK(packed.image_a_rgba16f[channel] == 0u &&
                  packed.image_b_rgba16f[channel] == 0u &&
                  packed.image_c_rgba8[channel] == 0u,
              "dry cells are zero-filled instead of leaking stale field values");
    }
}

void test_rejects_malformed_fields_before_allocation() {
    hydrology::GameplayFieldLayout huge{};
    huge.cell_size_m = 0.5f;
    huge.width = std::numeric_limits<std::uint32_t>::max();
    huge.depth = std::numeric_limits<std::uint32_t>::max();
    const std::vector<hydrology::GameplaySample> gameplay;
    const std::vector<hydrology::PresentationSample> presentation;
    viewer::PackedWaterField packed;
    viewer::WaterFieldError error;
    CHECK(!viewer::pack_water_field(
              {huge, &gameplay, &presentation, 1u, 2u}, packed, error) &&
              error.code == viewer::WaterFieldErrorCode::InvalidInput,
          "dimension multiplication overflow is rejected before allocation");

    hydrology::GameplayFieldLayout one{};
    one.cell_size_m = 1.0f;
    one.width = 1;
    one.depth = 1;
    std::vector<hydrology::GameplaySample> mismatched{{
        1.0f, 1.0f, 0.0f, 0.0f, 0.0f, true}};
    std::vector<hydrology::PresentationSample> dry{{
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        hydrology::RiverFeature::Calm, false}};
    CHECK(!viewer::pack_water_field(
              {one, &mismatched, &dry, 1u, 2u}, packed, error),
          "gameplay and presentation wet-valid masks must agree");
}

void test_eight_slots_replace_and_retire_transactionally() {
    viewer::WaterFieldVk table;
    viewer::WaterFieldError error;
    std::vector<viewer::WaterFieldBinding> handles;
    for (std::uint32_t index = 0; index != viewer::kWaterFieldBindingSlots;
         ++index) {
        auto packed = packed_one_cell(100u + index, 200u + index);
        viewer::WaterFieldBinding handle;
        CHECK(table.publish(packed, nullptr, 0u, handle, error),
              error.message.c_str());
        handles.push_back(handle);
    }
    CHECK(table.occupied_count() == viewer::kWaterFieldBindingSlots,
          "exactly eight immutable water bindings can be resident");
    viewer::WaterFieldBinding ninth;
    const auto overflow = packed_one_cell(999u, 1000u);
    CHECK(!table.publish(overflow, nullptr, 0u, ninth, error) &&
              error.code == viewer::WaterFieldErrorCode::Capacity,
          "the ninth binding fails explicitly without evicting a valid field");

    const viewer::WaterFieldBinding old = handles[3];
    const auto replacement = packed_one_cell(303u, 404u);
    viewer::WaterFieldBinding current;
    CHECK(table.publish(replacement, &old, 20u, current, error) &&
              current.slot == old.slot && current.generation > old.generation &&
              table.lookup(current) != nullptr && table.lookup(old) == nullptr,
          "replacement swaps one slot to a newer immutable generation");
    viewer::PackedWaterField malformed = replacement;
    malformed.image_c_rgba8.clear();
    viewer::WaterFieldBinding rejected;
    CHECK(!table.publish(malformed, &current, 21u, rejected, error) &&
              table.lookup(current) != nullptr,
          "failed replacement retains the last valid binding");

    CHECK(table.release(current, 30u, error), error.message.c_str());
    CHECK(!table.publish(overflow, nullptr, 0u, ninth, error),
          "a released slot is not reused while GPU work may reference it");
    table.collect(29u);
    CHECK(!table.publish(overflow, nullptr, 0u, ninth, error),
          "completion before the retirement serial keeps the slot quarantined");
    table.collect(30u);
    CHECK(table.publish(overflow, nullptr, 0u, ninth, error) &&
              ninth.slot == current.slot &&
              ninth.generation > current.generation,
          "the slot is reusable with a new generation after completion");
}

}  // namespace

int main() {
    test_packs_exact_three_image_contract();
    test_rejects_malformed_fields_before_allocation();
    test_eight_slots_replace_and_retire_transactionally();
    return check_summary();
}
