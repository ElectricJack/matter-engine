#include "check.h"
#include "../src/hydrology/river_network_builder.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using matter::RiverBoulders;
using matter::RiverChannel;
using matter::RiverFirstSection;
using matter::RiverInlet;
using matter::RiverNetworkDefinition;
using matter::RiverReach;
using hydrology::RiverNetworkBuilder;

std::uint64_t expected_fnv1a64(const std::string& text) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool build_valid_network(float second_grade, RiverNetworkDefinition& out,
                         std::string& error) {
    RiverNetworkBuilder builder(0.5f, 0x52495645u);
    std::size_t main = 0;
    return builder.add_river("main", main, error) &&
           builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error) &&
           builder.set_spline(main,
                              {{0.0f, 18.0f, 0.0f},
                               {34.0f, 14.0f, 11.0f},
                               {72.0f, 9.0f, -9.0f},
                               {128.0f, 3.0f, 5.0f}}, error) &&
           builder.add_reach(main, {64.0f, -0.035f, 0.15f, 0.75f}, error) &&
           builder.add_reach(main, {128.0f, second_grade, 0.65f, 1.35f}, error) &&
           builder.set_channel(main, {7.0f, 2.5f, 0.35f}, error) &&
           builder.set_boulders(main, {0.08f, {0.5f, 2.0f}}, error) &&
           builder.set_first_section(main,
                                     {100.0f, 4.0f, 0.80f, 32u, 256u,
                                      65536u}, error) &&
           builder.finish(out, error);
}

void test_records_in_insertion_order_and_keys_every_field() {
    RiverNetworkDefinition first;
    RiverNetworkDefinition same;
    RiverNetworkDefinition changed_grade;
    std::string error;
    CHECK(build_valid_network(-0.012f, first, error), error.c_str());
    error.clear();
    CHECK(build_valid_network(-0.012f, same, error), error.c_str());
    error.clear();
    CHECK(build_valid_network(-0.013f, changed_grade, error), error.c_str());

    CHECK(first.rivers.size() == 1u && first.rivers[0].name == "main",
          "the canonical builder retains named rivers in declaration order");
    CHECK(first.rivers[0].reaches.size() == 2u &&
              first.rivers[0].reaches[0].until_m == 64.0f &&
              first.rivers[0].reaches[1].meander == 0.65f &&
              first.rivers[0].reaches[0].width_scale == 0.75f &&
              first.rivers[0].reaches[1].width_scale == 1.35f,
          "reach declarations retain their authored order and values");
    CHECK(first.first_section_river == "main" &&
              first.first_section.minimum_length_m == 100.0f,
          "the first-section request names its river and keeps its minimum length");
    CHECK(first.canonical_text == same.canonical_text &&
              first.canonical_hash == same.canonical_hash,
          "identical declarations produce identical canonical bytes and keys");
    CHECK(first.canonical_hash == expected_fnv1a64(first.canonical_text),
          "the canonical key is FNV-1a-64 over the preserved canonical bytes");
    CHECK(first.canonical_text != changed_grade.canonical_text &&
              first.canonical_hash != changed_grade.canonical_hash,
          "base grade participates in canonical serialization and keying");
}

void test_rejects_duplicate_names_and_invalid_declarations() {
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t first = 0, duplicate = 0;
        std::string error;
        CHECK(builder.add_river("main", first, error), error.c_str());
        CHECK(!builder.add_river("main", duplicate, error) &&
                  error.find("hydrology.main.name") != std::string::npos,
              "duplicate river names are rejected at the named river path");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.set_inlet(
                  main, {{0.0f, std::numeric_limits<float>::infinity(), 0.0f},
                         1.0f}, error) &&
                  error.find("hydrology.main.inlet.position") != std::string::npos,
              "nonfinite inlet data is rejected at its canonical path");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.set_spline(main, {{0.0f, 0.0f, 0.0f}}, error) &&
                  error.find("hydrology.main.spline") != std::string::npos,
              "a spline shorter than two points is rejected");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(builder.add_reach(main, {64.0f, -0.02f, 0.2f}, error),
              error.c_str());
        CHECK(!builder.add_reach(main, {32.0f, -0.01f, 0.3f}, error) &&
                  error.find("hydrology.main.reach[1].until") !=
                      std::string::npos,
              "decreasing reach boundaries are rejected at the second boundary");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.add_reach(main, {64.0f, -0.02f, 0.2f, 0.0f}, error) &&
                  error.find("hydrology.main.reach[0].widthScale") !=
                      std::string::npos,
              "nonpositive width scales are rejected at the authored reach");
    }
    {
        RiverNetworkBuilder builder(0.5f, 1u);
        std::size_t main = 0;
        std::string error;
        CHECK(builder.add_river("main", main, error), error.c_str());
        CHECK(!builder.reserve_join(main, error) &&
                  error.find("hydrology.main.joins") != std::string::npos,
              "reserved tributary joins fail without retaining a declaration");
    }
}

void test_finish_is_single_use() {
    RiverNetworkBuilder builder(0.5f, 0x52495645u);
    RiverNetworkDefinition first;
    std::string error;
    std::size_t main = 0;
    CHECK(builder.add_river("main", main, error), error.c_str());
    CHECK(builder.set_inlet(main, {{0.0f, 18.0f, 0.0f}, 1.0f}, error),
          error.c_str());
    CHECK(builder.set_spline(main, {{0.0f, 18.0f, 0.0f},
                                    {128.0f, 3.0f, 5.0f}}, error),
          error.c_str());
    CHECK(builder.add_reach(main, {128.0f, -0.012f, 0.65f}, error),
          error.c_str());
    CHECK(builder.set_channel(main, {7.0f, 2.5f, 0.35f}, error),
          error.c_str());
    CHECK(builder.set_boulders(main, {0.08f, {0.5f, 2.0f}}, error),
          error.c_str());
    CHECK(builder.set_first_section(main,
                                    {100.0f, 4.0f, 0.80f, 32u, 256u,
                                     65536u}, error), error.c_str());
    CHECK(builder.finish(first, error), error.c_str());
    RiverNetworkDefinition repeated;
    error.clear();
    CHECK(!builder.finish(repeated, error) &&
              error.find("hydrology.build") != std::string::npos,
          "finish is single-use and rejects a repeated build");
}

} // namespace

int main() {
    test_records_in_insertion_order_and_keys_every_field();
    test_rejects_duplicate_names_and_invalid_declarations();
    test_finish_is_single_use();
    return check_summary();
}
