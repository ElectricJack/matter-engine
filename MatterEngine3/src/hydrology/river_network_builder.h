#pragma once

#include "matter/river_network.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hydrology {

class RiverNetworkBuilder {
public:
    RiverNetworkBuilder(float cell_size_m, std::uint64_t seed);

    bool add_river(const std::string& name, std::size_t& river,
                   std::string& error);
    bool set_inlet(std::size_t river, const matter::RiverInlet& inlet,
                   std::string& error);
    bool set_spline(std::size_t river, const std::vector<matter::Float3>& spline,
                    std::string& error);
    bool add_reach(std::size_t river, const matter::RiverReach& reach,
                   std::string& error);
    bool set_channel(std::size_t river, const matter::RiverChannel& channel,
                     std::string& error);
    bool set_boulders(std::size_t river, const matter::RiverBoulders& boulders,
                      std::string& error);
    bool reserve_join(std::size_t river, std::string& error);
    bool set_first_section(std::size_t river, const matter::RiverFirstSection& section,
                           std::string& error);

    bool finish(matter::RiverNetworkDefinition& out, std::string& error);

private:
    struct RiverState {
        matter::RiverDefinition definition;
        bool has_inlet = false;
        bool has_spline = false;
        bool has_channel = false;
        bool has_boulders = false;
    };

    bool mutable_river(std::size_t river, RiverState*& out, std::string& error);
    std::string river_path(std::size_t river) const;

    float cell_size_m_ = 0.0f;
    std::uint64_t seed_ = 0;
    std::vector<RiverState> rivers_;
    std::size_t first_section_river_ = 0;
    matter::RiverFirstSection first_section_{};
    bool has_first_section_ = false;
    bool finished_ = false;
};

} // namespace hydrology
