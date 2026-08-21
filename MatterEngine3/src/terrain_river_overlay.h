#pragma once

#include "hydrology/river_geometry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace terrain_field {

class HeightOverlay {
public:
    virtual ~HeightOverlay() = default;
    virtual float height_at(float x, float z, float base_height) const = 0;
    virtual std::uint64_t hash() const = 0;
};

class RiverHeightOverlay final : public HeightOverlay {
public:
    static bool build(const hydrology::RiverGeometry& geometry,
                      const matter::RiverChannel& channel,
                      std::shared_ptr<const RiverHeightOverlay>& out,
                      std::string& error);

    float height_at(float x, float z, float base_height) const override;
    std::uint64_t hash() const override { return hash_; }

private:
    struct Sample {
        float x = 0.0f;
        float z = 0.0f;
        float lateral_x = 0.0f;
        float lateral_z = 0.0f;
        float distance_m = 0.0f;
        float thalweg_y = 0.0f;
    };
    struct Boulder {
        float x = 0.0f;
        float z = 0.0f;
        float bed_y = 0.0f;
        float radius_m = 0.0f;
    };

    float terrain_height(float x, float z, float base_height) const;
    std::size_t nearest_sample(float x, float z) const;

    matter::RiverChannel channel_{};
    std::vector<Sample> samples_;
    std::vector<Boulder> boulders_;
    std::uint64_t terrain_seed_ = 0;
    std::uint64_t hash_ = 0;
};

} // namespace terrain_field
