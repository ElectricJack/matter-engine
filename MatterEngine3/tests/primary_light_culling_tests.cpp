#include "check.h"
#include "../src/render/primary_light_culling.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace {
using namespace world_lights;

LocalLight light_at(float x, float y, float z, float range) {
    LocalLight light{};
    light.position[0] = x; light.position[1] = y; light.position[2] = z;
    light.range = range;
    light.source_radius = 0.1f;
    light.direction[2] = 1.0f;
    light.cos_inner = 0.9f;
    light.cos_outer = 0.7f;
    return light;
}

void allocation_contract() {
    const auto unlimited = std::numeric_limits<std::uint64_t>::max();
    auto layout = primary_light_mask_layout(37, 29, 65, unlimited, unlimited);
    CHECK(layout.enabled && layout.tiles_x == 3 && layout.tiles_y == 2 &&
          layout.words_per_tile == 3 && layout.byte_size == 1704,
          "partial 37x29 tiles include 65 light bits and 66 list words each");
    CHECK(primary_light_mask_layout(37, 29, 65, 1704, 1704).enabled,
          "exact allocation budget fits");
    CHECK(!primary_light_mask_layout(37, 29, 65, 1703, unlimited).enabled,
          "storage budget overflow falls back");
    CHECK(!primary_light_mask_layout(37, 29, 65, unlimited, 1703).enabled,
          "buffer budget overflow falls back");
    layout = primary_light_mask_layout(37, 29, 0, unlimited, unlimited);
    CHECK(!layout.enabled && layout.byte_size == 48 && layout.words_per_tile == 0,
          "zero lights retains metadata-only spatial fallback");
    CHECK(!primary_light_mask_layout(0, 29, 32, unlimited, unlimited).enabled &&
          !primary_light_mask_layout(37, 0, 32, unlimited, unlimited).enabled,
          "zero extent disables masks");
    CHECK(!primary_light_mask_layout(16, 16, 1, 47, unlimited).enabled,
          "metadata below device budget disables masks");
    CHECK(primary_light_mask_layout(16, 16, 32, unlimited, unlimited).byte_size == 184 &&
          primary_light_mask_layout(16, 16, 33, unlimited, unlimited).byte_size == 192,
          "mask word boundary and compact list sizing");
    CHECK(!primary_light_mask_layout(37, 29, 65, 120, unlimited).enabled,
          "a budget fitting only masks cannot truncate compact lists");
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    CHECK(!primary_light_mask_layout(maximum, maximum, maximum, unlimited, unlimited).enabled,
          "max dimensions and lights cannot overflow allocation arithmetic");
    CHECK(!primary_light_mask_layout(maximum, maximum, 1, unlimited, unlimited).enabled,
          "32-bit shader addressing overflow falls back");
    layout = primary_light_mask_layout(1, 1, maximum, unlimited, unlimited);
    CHECK(!layout.enabled && layout.byte_size == 48 && layout.words_per_tile == 0,
          "maximum light count plus count word cannot wrap into a small allocation");
    // One light needs three words per tile: mask, count, and ID. This is the
    // last whole tile that fits the 64 MiB policy, with four bytes to spare.
    constexpr std::uint32_t budget_tiles = (64u * 1024u * 1024u - 48u) / 12u;
    layout = primary_light_mask_layout(budget_tiles * 16u, 16, 1, unlimited, unlimited);
    CHECK(layout.enabled && layout.byte_size == 67108860u,
          "largest one-light tile allocation below 64 MiB fits");
    CHECK(!primary_light_mask_layout((budget_tiles + 1u) * 16u, 16, 1,
                                    unlimited, unlimited).enabled,
          "64 MiB cap applies even with unlimited device limits");
}

void edge_contract() {
    PrimaryLightAabb box{{-1,-1,-1}, {1,1,1}};
    auto light = light_at(10,0,0,1);
    CHECK(!primary_light_bounds_may_intersect(light, box), "distant point rejected");
    light.source_radius = 1000;
    CHECK(!primary_light_bounds_may_intersect(light, box), "source radius does not enlarge cutoff");
    light = light_at(2,0,0,1);
    CHECK(primary_light_bounds_may_intersect(light, box), "range tangent retained");
    light = light_at(0,0,0,100);
    light.kind = static_cast<std::uint32_t>(LocalLightKind::Spot);
    CHECK(primary_light_bounds_may_intersect(light, box), "cone apex retained");
    PrimaryLightAabb behind{{-0.01f,-0.01f,-5.01f}, {0.01f,0.01f,-4.99f}};
    CHECK(!primary_light_bounds_may_intersect(light, behind), "distant cone back rejected");
    light.cos_outer = light.cos_inner = 0.0f;
    PrimaryLightAabb tangent{{5,0,0},{5,0,0}};
    CHECK(primary_light_bounds_may_intersect(light, tangent), "hemisphere tangent retained");
    light.cos_outer = light.cos_inner = -0.5f;
    PrimaryLightAabb wide{{4,0,-1},{4,0,-1}};
    CHECK(local_light_attenuation(light, wide.min) > 0 &&
          primary_light_bounds_may_intersect(light, wide), "obtuse cone witness retained");
    light = light_at(0,0,0,1.0e20f);
    PrimaryLightAabb huge{{1.0e19f,0,0},{1.0e19f,0,0}};
    CHECK(local_light_attenuation(light, huge.min) > 0 &&
          primary_light_bounds_may_intersect(light, huge), "huge finite range witness retained");
    light.range = 1.0e-8f;
    PrimaryLightAabb tiny{{1.0e-9f,0,0},{1.0e-9f,0,0}};
    CHECK(local_light_attenuation(light, tiny.min) > 0 &&
          primary_light_bounds_may_intersect(light, tiny), "tiny range witness retained");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    light.range = inf;
    CHECK(primary_light_bounds_may_intersect(light, box), "nonfinite range retained");
    light = light_at(nan,0,0,1);
    CHECK(primary_light_bounds_may_intersect(light, box), "nonfinite position retained");
    light = light_at(0,0,0,100);
    light.kind = static_cast<std::uint32_t>(LocalLightKind::Spot);
    light.direction[2] = 0;
    CHECK(primary_light_bounds_may_intersect(light, behind), "zero axis retained");
    light.direction[2] = nan;
    CHECK(primary_light_bounds_may_intersect(light, behind), "nonfinite axis retained");
    light.direction[2] = 1;
    light.cos_inner = nan;
    CHECK(primary_light_bounds_may_intersect(light, behind), "nonfinite cone retained");
    box.min[0] = nan;
    CHECK(primary_light_bounds_may_intersect(light, box), "nonfinite receiver retained");
    const float maximum = std::numeric_limits<float>::max();
    light = light_at(0,0,0,maximum);
    PrimaryLightAabb overflow{{maximum,0,0},{maximum,0,0}};
    CHECK(primary_light_bounds_may_intersect(light, overflow), "overflowed receiver padding retained");
    light = light_at(0,0,0,1.0e-30f);
    CHECK(primary_light_bounds_may_intersect(light, behind), "overflowed normalized distance retained");
    PrimaryLightAabb inverted{{1,0,0},{-1,0,0}};
    CHECK(primary_light_bounds_may_intersect(light, inverted), "inverted bounds retained");
}

void witness_property() {
    std::mt19937 random(0x54317u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto signed_unit = [&]() { return 2.0f * unit(random) - 1.0f; };
    unsigned witnesses = 0, rejected = 0;
    // Bounded deterministic sampling covers translated tiles, random cone axes,
    // soft/hard/reversed edges, near-zero through huge ranges, and nonunit axes.
    for (unsigned trial = 0; trial < 20000; ++trial) {
        const float scale = std::pow(10.0f, -6.0f + 24.0f * unit(random));
        auto light = light_at(signed_unit()*scale, signed_unit()*scale,
                              signed_unit()*scale, scale*(0.2f+unit(random)));
        light.kind = trial % 3 == 0 ? 0u : 1u;
        float length2 = 0;
        for (float& axis : light.direction) { axis = signed_unit(); length2 += axis*axis; }
        const float length = std::sqrt(length2);
        const float axis_scale = trial % 7 == 0 ? 1.00008f : 1.0f;
        for (float& axis : light.direction) axis *= axis_scale / length;
        light.cos_outer = signed_unit();
        light.cos_inner = trial % 4 == 0 ? light.cos_outer : signed_unit();
        PrimaryLightAabb bounds{};
        for (int axis = 0; axis < 3; ++axis) {
            const float center = light.position[axis] + signed_unit()*scale*1.5f;
            const float extent = unit(random)*scale*0.25f;
            bounds.min[axis] = center - extent;
            bounds.max[axis] = center + extent;
        }
        const bool kept = primary_light_bounds_may_intersect(light, bounds);
        if (!kept) ++rejected;
        for (unsigned sample = 0; sample < 16; ++sample) {
            float receiver[3];
            for (int axis = 0; axis < 3; ++axis) {
                const float t = sample < 8 ? float((sample >> axis) & 1u) : unit(random);
                receiver[axis] = bounds.min[axis]*(1-t) + bounds.max[axis]*t;
            }
            if (local_light_attenuation(light, receiver) > 0.0f) {
                ++witnesses;
                CHECK(kept, "positive attenuation witness must survive tile rejection");
            }
        }
    }
    CHECK(witnesses > 1000, "property exercised positive attenuation witnesses");
    CHECK(rejected > 1000, "property rules out an always-true implementation");
    std::printf("primary tile witnesses=%u rejected_boxes=%u\n", witnesses, rejected);
}
} // namespace

int main() {
    allocation_contract();
    edge_contract();
    witness_property();
    return check_summary();
}
