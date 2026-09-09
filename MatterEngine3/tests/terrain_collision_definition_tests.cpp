#include "check.h"
#include "../src/bake_mode.h"
#include "../src/terrain_collision/terrain_collision_definition.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using matter::Float3;
using matter::TerrainCollisionDefinition;
using matter::TerrainCollisionRegion;
using matter::terrain_collision::CanonicalDefinition;
using matter::terrain_collision::SectorCoordinate;
using matter::terrain_collision::SourceIdentity;

TerrainCollisionRegion region(const char* id, Float3 min_m, Float3 max_m) {
    TerrainCollisionRegion result;
    result.id = id;
    result.min_m = min_m;
    result.max_m = max_m;
    return result;
}

TerrainCollisionDefinition definition(std::vector<TerrainCollisionRegion> regions) {
    TerrainCollisionDefinition result;
    result.cell_size_m = 0.5f;
    result.rung = 2;
    result.friction = 0.72f;
    result.restitution = 0.02f;
    result.regions = std::move(regions);
    return result;
}

SourceIdentity source() {
    SourceIdentity result;
    result.field_hash = 0x1111222233334444ULL;
    result.overlay_hash = 0x5555666677778888ULL;
    result.bake_mode_salt = bake_mode::salt();
    result.mesher_semantic_version = 1;
    result.geometry_format_version = 1;
    return result;
}

bool canonicalize_definition(const TerrainCollisionDefinition& input,
                             float sector_size_m,
                             const SourceIdentity& input_source,
                             CanonicalDefinition& output,
                             std::string& error) {
    return matter::terrain_collision::canonicalize(
        input, sector_size_m, input_source, output, error);
}

bool contains(const std::string& text, const char* expected) {
    return text.find(expected) != std::string::npos;
}

void test_cell_size_ladder_is_exact() {
    struct Case { float cell_size_m; std::int8_t rung; };
    const Case accepted[] = {
        {0.25f, 3}, {0.5f, 2}, {1.0f, 1}, {2.0f, 0}, {4.0f, -1},
        {8.0f, -2}, {16.0f, -3}, {32.0f, -4}, {64.0f, -5},
    };
    for (const Case& item : accepted) {
        std::int8_t rung = 99;
        CHECK(matter::terrain_collision::cell_size_to_rung(item.cell_size_m, rung),
              "every documented cell size is accepted");
        CHECK(rung == item.rung, "each documented cell size maps to its exact rung");
    }

    const float rejected[] = {
        0.0f, -0.5f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), 0.25000003f, 0.75f, 3.0f,
    };
    for (float value : rejected) {
        std::int8_t rung = 99;
        CHECK(!matter::terrain_collision::cell_size_to_rung(value, rung),
              "unsupported cell sizes are rejected without rounding");
    }
}

void test_canonicalizes_overlap_order_and_half_open_grid() {
    const std::vector<TerrainCollisionRegion> ordered = {
        region("west", {-64.0f, -64.0f, -64.0f}, {64.0f, 64.0f, 64.0f}),
        region("east", {0.0f, -64.0f, -64.0f}, {128.0f, 64.0f, 64.0f}),
    };
    const std::vector<TerrainCollisionRegion> reordered = {
        region("overlap", {0.0f, -64.0f, -64.0f}, {128.0f, 64.0f, 64.0f}),
        region("base", {-64.0f, -64.0f, -64.0f}, {64.0f, 64.0f, 64.0f}),
    };
    CanonicalDefinition first;
    CanonicalDefinition second;
    std::string error;
    CHECK(canonicalize_definition(definition(ordered), 64.0f, source(), first, error),
          error.c_str());
    CHECK(canonicalize_definition(definition(reordered), 64.0f, source(), second, error),
          error.c_str());
    CHECK(first.sectors == second.sectors, "authoring order canonicalizes to one sorted union");
    CHECK(first.geometry_key == second.geometry_key,
          "authoring order and labels do not change geometry identity");
    CHECK(first.installation_key == second.installation_key,
          "authoring order and labels do not change installation identity");

    const std::vector<SectorCoordinate> expected = {
        {-1, -1, -1}, {-1, -1, 0}, {-1, 0, -1}, {-1, 0, 0},
        {0, -1, -1}, {0, -1, 0}, {0, 0, -1}, {0, 0, 0},
        {1, -1, -1}, {1, -1, 0}, {1, 0, -1}, {1, 0, 0},
    };
    CHECK(first.sectors == expected,
          "regions enumerate min-inclusive max-exclusive sectors in x/y/z order");
}

void test_rejects_invalid_definition_fields() {
    const TerrainCollisionRegion valid =
        region("valid", {0.0f, 0.0f, 0.0f}, {64.0f, 64.0f, 64.0f});
    struct Case {
        TerrainCollisionDefinition input;
        float sector_size_m;
        const char* path;
    };
    TerrainCollisionDefinition empty_regions = definition({});
    TerrainCollisionDefinition empty_id = definition({valid});
    empty_id.regions[0].id.clear();
    TerrainCollisionDefinition duplicate_id = definition({valid, valid});
    TerrainCollisionDefinition nonfinite_bound = definition({valid});
    nonfinite_bound.regions[0].min_m.x = std::numeric_limits<float>::infinity();
    TerrainCollisionDefinition bad_range = definition({valid});
    bad_range.regions[0].max_m.z = 0.0f;
    TerrainCollisionDefinition misaligned = definition({valid});
    misaligned.regions[0].min_m.y = 0.5f;
    TerrainCollisionDefinition misaligned_x = definition({valid});
    misaligned_x.regions[0].min_m.x = 0.5f;
    TerrainCollisionDefinition misaligned_z = definition({valid});
    misaligned_z.regions[0].max_m.z = 63.5f;
    TerrainCollisionDefinition bad_cell = definition({valid});
    bad_cell.cell_size_m = 0.75f;
    TerrainCollisionDefinition bad_friction = definition({valid});
    bad_friction.friction = 1.01f;
    TerrainCollisionDefinition bad_restitution = definition({valid});
    bad_restitution.restitution = -0.01f;
    const Case cases[] = {
        {empty_regions, 64.0f, "regions"}, {empty_id, 64.0f, "region"},
        {duplicate_id, 64.0f, "duplicate"}, {nonfinite_bound, 64.0f, "min"},
        {bad_range, 64.0f, "max"}, {misaligned_x, 64.0f, "aligned"},
        {misaligned, 64.0f, "aligned"}, {misaligned_z, 64.0f, "aligned"},
        {bad_cell, 64.0f, "cellSize"}, {bad_friction, 64.0f, "friction"},
        {bad_restitution, 64.0f, "restitution"}, {definition({valid}), 0.0f, "sector"},
        {definition({valid}), std::numeric_limits<float>::infinity(), "sector"},
    };
    for (const Case& item : cases) {
        CanonicalDefinition output;
        std::string error;
        CHECK(!canonicalize_definition(item.input, item.sector_size_m, source(), output, error),
              "each invalid definition is rejected");
        CHECK(contains(error, item.path), "validation error identifies the failing field");
    }
}

void test_rejects_sector_union_larger_than_documented_limit() {
    const TerrainCollisionRegion too_many = region(
        "too-many", {0.0f, 0.0f, 0.0f}, {64000064.0f, 64.0f, 64.0f});
    CanonicalDefinition output;
    std::string error;
    CHECK(!canonicalize_definition(definition({too_many}), 64.0f, source(), output, error),
          "a sector union larger than the documented limit is rejected before allocation");
    CHECK(contains(error, "kMaxSectorCount"),
          "the sector limit error names the implementation limit");
}

void test_rejects_out_of_range_sector_quotients_without_narrowing() {
    const float two_to_63 = std::ldexp(1.0f, 63);
    const float greatest_in_range = std::nextafter(two_to_63, 0.0f);
    const auto run = [](const TerrainCollisionRegion& input, const char* path) {
        CanonicalDefinition output;
        std::string error;
        CHECK(!canonicalize_definition(definition({input}), 1.0f, source(), output, error),
              "out-of-range or impractically large sector quotient is rejected");
        CHECK(contains(error, path), "sector quotient rejection identifies its boundary");
    };
    run(region("upper", {0.0f, 0.0f, 0.0f}, {two_to_63, 1.0f, 1.0f}),
        "aligned");
    run(region("in-range", {0.0f, 0.0f, 0.0f}, {greatest_in_range, 1.0f, 1.0f}),
        "kMaxSectorCount");
    run(region("lower", {-two_to_63, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}),
        "kMaxSectorCount");
}

void test_identity_tracks_semantic_inputs_only() {
    const auto base = definition({
        region("one", {0.0f, 0.0f, 0.0f}, {64.0f, 64.0f, 64.0f}),
        region("two", {64.0f, 0.0f, 0.0f}, {128.0f, 64.0f, 64.0f}),
    });
    CanonicalDefinition canonical_base;
    std::string error;
    CHECK(canonicalize_definition(base, 64.0f, source(), canonical_base, error), error.c_str());

    auto labels = base;
    labels.regions[0].id = "renamed-one";
    labels.regions[1].id = "renamed-two";
    CanonicalDefinition canonical_labels;
    CHECK(canonicalize_definition(labels, 64.0f, source(), canonical_labels, error), error.c_str());
    CHECK(canonical_labels.geometry_key == canonical_base.geometry_key,
          "region labels do not affect geometry identity");
    CHECK(canonical_labels.installation_key == canonical_base.installation_key,
          "region labels do not affect installation identity");

    auto material = base;
    material.friction = 0.4f;
    material.restitution = 0.3f;
    CanonicalDefinition canonical_material;
    CHECK(canonicalize_definition(material, 64.0f, source(), canonical_material, error), error.c_str());
    CHECK(canonical_material.geometry_key == canonical_base.geometry_key,
          "material-only changes reuse geometry identity");
    CHECK(canonical_material.installation_key != canonical_base.installation_key,
          "material-only changes replace installation identity");

    const auto assert_geometry_change = [&](TerrainCollisionDefinition input,
                                            float sector_size_m,
                                            SourceIdentity input_source) {
        CanonicalDefinition changed;
        CHECK(canonicalize_definition(input, sector_size_m, input_source, changed, error), error.c_str());
        CHECK(changed.geometry_key != canonical_base.geometry_key,
              "a geometry input changes geometry identity");
    };
    SourceIdentity changed_source = source();
    ++changed_source.field_hash;
    assert_geometry_change(base, 64.0f, changed_source);
    changed_source = source();
    ++changed_source.overlay_hash;
    assert_geometry_change(base, 64.0f, changed_source);
    changed_source = source();
    ++changed_source.mesher_semantic_version;
    assert_geometry_change(base, 64.0f, changed_source);
    changed_source = source();
    changed_source.bake_mode_salt = 0xC0470552EA3D0001ULL;
    assert_geometry_change(base, 64.0f, changed_source);
    auto changed_cell = base;
    changed_cell.cell_size_m = 1.0f;
    changed_cell.rung = 1;
    assert_geometry_change(changed_cell, 64.0f, source());
    const auto changed_sector_size = definition({
        region("sector-size", {0.0f, 0.0f, 0.0f}, {128.0f, 128.0f, 128.0f}),
    });
    assert_geometry_change(changed_sector_size, 128.0f, source());
    auto changed_union = base;
    changed_union.regions[1].max_m.x = 256.0f;
    assert_geometry_change(changed_union, 64.0f, source());
}

}  // namespace

int main() {
    const int previous_mode = bake_mode::forced_contour_seams();
    bake_mode::forced_contour_seams() = 1;
    test_cell_size_ladder_is_exact();
    test_canonicalizes_overlap_order_and_half_open_grid();
    test_rejects_invalid_definition_fields();
    test_rejects_sector_union_larger_than_documented_limit();
    test_rejects_out_of_range_sector_quotients_without_narrowing();
    test_identity_tracks_semantic_inputs_only();
    bake_mode::forced_contour_seams() = previous_mode;
    return check_summary();
}
