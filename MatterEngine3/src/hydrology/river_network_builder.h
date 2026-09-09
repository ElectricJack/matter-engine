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
    bool set_curve(std::size_t river, const std::vector<matter::Float3>& curve,
                   std::string& error);
    bool set_channel_profile(
        std::size_t river,
        const std::vector<matter::RiverChannelProfilePoint>& profile,
        std::string& error);
    bool add_section(std::size_t river, const std::string& id,
                     float from_m, float to_m, float dry_margin_m,
                     std::size_t& section, std::string& error);
    bool set_section_emitters(std::size_t section,
                              const std::vector<std::string>& emitter_ids,
                              std::string& error);
    bool add_section_waterfall(
        std::size_t section,
        const matter::RiverWaterfallDefinition& waterfall,
        std::string& error);
    bool set_section_pool(std::size_t section,
                          const matter::RiverPoolDefinition& pool,
                          std::string& error);
    bool set_section_spillway(
        std::size_t section,
        const matter::RiverSpillwayDefinition& spillway,
        std::string& error);
    bool add_section_after(std::size_t section, const std::string& upstream,
                           std::string& error);
    bool add_section_from_spillway(std::size_t section,
                                   const std::string& upstream,
                                   std::string& error);
    bool set_bake_sequential(std::string& error);
    bool reserve_join(std::size_t river, std::string& error);
    bool set_backend(matter::HydrologyBackend backend, std::string& error);
    bool set_pbd(const matter::HydrologyPbdSettings& settings,
                 std::string& error);
    bool set_mesh_animation(
        const matter::HydrologyMeshAnimationProfile& profile,
        std::string& error);
    bool set_limits(const matter::HydrologyBakeLimits& limits,
                    std::string& error);
    bool set_escape_policy(const matter::HydrologyEscapePolicy& policy,
                           std::string& error);
    bool add_emitter(const matter::HydrologyEmitter& emitter,
                     std::string& error);
    bool set_virtual_dam(const matter::HydrologyVirtualDam& dam,
                         std::string& error);
    bool set_fill_sensor(const matter::HydrologyFillSensor& sensor,
                         std::string& error);
    bool set_quality(const matter::HydrologyQualitySettings& quality,
                     std::string& error);
    bool set_water_material(std::uint32_t material_id, std::string& error);
    bool set_water_optics(const matter::WaterOpticalDefinition& optics,
                          std::string& error);
    bool add_water_wave_band(const matter::WaterWaveBandDefinition& wave,
                             std::string& error);
    bool set_water_foam(const matter::WaterFoamDefinition& foam,
                        std::string& error);
    bool add_water_local_override(
        const matter::WaterLocalOverrideDefinition& local,
        std::string& error);

    bool finish(matter::RiverNetworkDefinition& out, std::string& error);

private:
    struct RiverState {
        matter::RiverDefinition definition;
        bool has_inlet = false;
        bool has_curve = false;
        bool has_channel_profile = false;
    };

    struct SectionState {
        matter::RiverSectionDefinition definition;
        bool has_emitters = false;
    };

    bool mutable_river(std::size_t river, RiverState*& out, std::string& error);
    bool mutable_section(std::size_t section, SectionState*& out,
                         std::string& error);
    std::string river_path(std::size_t river) const;

    float cell_size_m_ = 0.0f;
    std::uint64_t seed_ = 0;
    std::vector<RiverState> rivers_;
    std::vector<SectionState> sections_;
    matter::HydrologyFluidRequest fluid_{};
    matter::WaterSurfaceDefinition water_surface_{};
    bool bake_sequential_ = false;
    bool has_backend_ = false;
    bool has_pbd_ = false;
    bool has_mesh_animation_ = false;
    bool has_limits_ = false;
    bool has_escape_policy_ = false;
    bool has_virtual_dam_ = false;
    bool has_fill_sensor_ = false;
    bool has_quality_ = false;
    bool has_water_material_ = false;
    bool has_water_optics_ = false;
    bool has_water_foam_ = false;
    bool finished_ = false;
};

} // namespace hydrology
