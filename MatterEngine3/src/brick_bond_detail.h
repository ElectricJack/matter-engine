#pragma once
#include "brick_bond_atlas.h"
#include "script_host.h"
#include <string>
namespace detail_bake {
constexpr uint32_t brick_bond_detail_version = 1;
struct BrickBondDescriptor {
    std::string source_module;
    std::string source_params_json = "{}";
    uint32_t variants = 8;
    castle_bake::BrickBondRecipe bond{};
    float pixel_m = .003f, padding_m = .01f;
    float hit_epsilon_m = .000005f, normal_epsilon_m = .00002f;
};
// Input is ScriptHost's merged static params JSON. Unknown kinds return true
// with recognized=false; a recognized but malformed descriptor fails closed.
bool parse_brick_bond_descriptor(const std::string &, BrickBondDescriptor &, bool &recognized,
                                 std::string &error);
struct PreparedBrickBond {
    std::array<script_host::EvaluatedSolidSource, 8> sources;
    uint64_t cache_key = 0;
    double source_evaluation_ms = 0;
};
// Evaluates each seed once in memory and derives identity before any projection.
bool prepare_brick_bond_sources(const BrickBondDescriptor &, uint64_t descriptor_hash,
                                const std::string &source_code, script_host::ScriptHost &,
                                const script_host::SolidSourceEvaluationOptions &,
                                PreparedBrickBond &, std::string &error);
gpu_meshing::FaceJob brick_bond_face_job(const BrickBondDescriptor &, const gpu_meshing::SolidJob &,
                                         uint64_t source_identity, bool back);
} // namespace detail_bake
