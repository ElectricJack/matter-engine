#include "surface_internal.h"
#include <matter/geometry/terrain_field.h>
#include <matter/asset/surface_tape.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

namespace matter::asset {
namespace {
using namespace detail;
using V = data::Vec3;
V mul(V a, float b) { return {a.x*b, a.y*b, a.z*b}; }
V unit(V a) { return mul(a, 1/std::sqrt(dot(a,a))); }
bool finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// The native compatibility parser accepts trailing numeric/token prefixes.
// Strict validation belongs at this public boundary, not in the shared parser.
void strict_tape(const std::string &text) {
  static const std::map<std::string, size_t> arities = {
      {"const",2}, {"input",2}, {"footprint",1}, {"noise2",6}, {"ridge2",6},
      {"noise2w",6}, {"ridge2w",6}, {"noise3",6}, {"ridge3",6}, {"noise3w",6}, {"ridge3w",6},
      {"cell2",4}, {"cellular3",6}, {"curv",2}, {"add",3}, {"sub",3}, {"mul",3},
      {"min",3}, {"max",3}, {"clamp",4}, {"blend",4}, {"smoothstep",4},
      {"abs",2}, {"oneminus",2}, {"pow",3}, {"fract",2}, {"material",3},
      {"source",11}, {"tint",4}, {"roughbias",2}, {"wetness",2}, {"metallic",2}, {"coat",6}};
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words(line);
    std::vector<std::string> tokens;
    std::string word;
    while (words >> word) tokens.push_back(word);
    if (tokens.empty()) continue;
    const auto &op = tokens[0];
    auto arity = arities.find(op);
    need(arity != arities.end(), "unsupported surface operation: " + op, E::unsupported_capability);
    const bool noise3 = op == "noise3" || op == "ridge3" || op == "noise3w" || op == "ridge3w";
    const bool noise = noise3 || op == "noise2" || op == "ridge2" || op == "noise2w" || op == "ridge2w";
    need(tokens.size() == arity->second || (noise3 && tokens.size() == 9), "surface operation has incorrect arity: " + op);
    for (size_t i = 1; i < tokens.size(); ++i) {
      const auto &t = tokens[i];
      if (op == "input" || (op == "cellular3" && i == 2)) continue;
      if (t[0] == 'r') {
        need(t.size() > 1 && t.find_first_not_of("0123456789", 1) == std::string::npos,
             "invalid surface register token");
        continue;
      }
      const bool integer = (noise && (i == 1 || i == 3 || i == 6)) ||
          ((op == "material" || op == "source" || op == "cell2" || op == "cellular3") && i == 1);
      try {
        size_t used = 0;
        if (integer) {
          need(t.find_first_not_of("0123456789") == std::string::npos, "expected unsigned integer surface token");
          const auto n = std::stoull(t, &used);
          need(n <= UINT32_MAX, "surface integer exceeds uint32");
          if (noise && i == 3) need(n >= 1 && n <= 16, "surface octaves must be in [1,16]");
          if (op == "source") need(n == 1, "only local direct-source v1 can be baked", E::unsupported_version);
        } else {
          const float n = std::stof(t, &used);
          need(std::isfinite(n), "nonfinite surface literal");
        }
        need(used == t.size(), "trailing bytes in surface numeric token");
      } catch (const Failure &) { throw; }
      catch (const std::exception &) { throw Failure(E::invalid_input, "invalid surface numeric token"); }
    }
  }
}

uint64_t program_cost(const terrain_field::SurfaceProgram &p, double reach) {
  uint64_t cost = 0;
  for (const auto &op : p.ops) {
    using O = terrain_field::Op;
    const bool noise = op.kind == O::Noise2 || op.kind == O::Ridge2 || op.kind == O::Noise3 || op.kind == O::Ridge3;
    if (noise) {
      need(op.f0 >= 0 && op.f1 >= 0 && op.f1 <= 1 && op.f2 > 0 && op.wf0 >= 0,
           "invalid surface noise frequency, gain, lacunarity or warp");
      float frequency = op.f0;
      const double warped = reach + std::abs(double(op.wf1)) * 2;
      need(!op.warp || reach * op.wf0 < 536870912.0, "surface warp exceeds safe lattice domain", E::unsupported_capability);
      for (int i = 0; i < op.oct; ++i) {
        need(std::isfinite(frequency) && warped * frequency < 536870912.0,
             "surface noise exceeds safe lattice domain", E::unsupported_capability);
        if (i + 1 < op.oct) frequency *= op.f2;
      }
      cost += uint64_t(op.oct) + (op.warp ? 3 : 0);
    } else {
      cost += op.kind == O::Cellular3 ? 125 : 1;
    }
    need(op.kind != O::Clamp || op.f0 <= op.f1, "invalid surface clamp interval");
    need(op.kind != O::Smoothstep || op.f0 < op.f1, "invalid surface smoothstep interval");
  }
  return cost * 6; // source, appearance and up to four height probes.
}
} // namespace

SurfaceResult bake_surface_tape(const services::JobEnvelope &job, const SurfaceTapeRequest &r,
                                const services::EvaluationOptions &o, const services::JobControl &control) {
  return detail::boundary<SurfaceResult>(job, [&] {
    services::JobMonitor monitor(o.budget, control);
    checked(monitor.checkpoint());
    const auto count = surface_preflight(r.settings, r.points.size(), o,
        {{surface_bake_capability, 1}, {surface_tape_capability, 1}});
    need(!r.tape.empty() && r.tape.size() <= 65536, "surface tape exceeds 64 KiB", E::resource_limit);
    services::ResourceUsage usage;
    usage.operations = r.tape.size();
    usage.working_bytes = uint64_t(r.tape.size()) * 16 + 3 * terrain_field::kMaxSurfaceSourceOps * sizeof(terrain_field::Op) +
        count * (sizeof(SurfaceTexel) + 53);
    checked(monitor.checkpoint(usage));
    strict_tape(r.tape);
    terrain_field::SurfaceProgram program;
    std::string error;
    const bool parsed = terrain_field::SurfaceProgram::parse(r.tape, program, error);
    need(parsed, "surface tape: " + error);
    need(program.source.version == 1 && !program.uses_world_inputs() &&
             !(program.input_mask() & (1u << terrain_field::kSurfInReceiverMaterial)),
         "surface producer needs a receiver-independent local direct source; classifier/world/receiver inputs require an adapter",
         E::unsupported_capability);
    need(std::abs(program.source.height_min) <= 1000 && std::abs(program.source.height_max) <= 1000,
         "surface relief exceeds supported +/-1000 metre domain", E::unsupported_capability);
    const bool relief = program.source.height_max > program.source.height_min;
    double reach = 1;
    for (size_t i = 0; i < r.points.size(); ++i) {
      if (!(i % 256)) checked(monitor.checkpoint());
      const auto &point = r.points[i];
      const V t{point.tangent.x, point.tangent.y, point.tangent.z};
      need(finite(point.position) && finite(point.normal) && finite(t) &&
               std::abs(dot(point.normal, point.normal)-1) <= 1e-4f &&
               std::abs(dot(t,t)-1) <= 1e-4f && std::abs(dot(t,point.normal)) <= 1e-4f &&
               (point.tangent.w == 1 || point.tangent.w == -1) && point.coverage <= 1 &&
               std::isfinite(point.footprint_metres) && point.footprint_metres > 0 && point.footprint_metres <= 1,
           "invalid surface hit frame, coverage or footprint");
      const float eps = std::max(.5f*point.footprint_metres, .0001f);
      for (float v : {point.position.x, point.position.y, point.position.z}) {
        need(std::abs(v) <= 1000000, "surface position exceeds local metre domain", E::unsupported_capability);
        if (relief && point.coverage)
          need(std::nextafter(std::abs(v), std::numeric_limits<float>::infinity()) - std::abs(v) <= eps / 4,
               "surface position loses relief probe precision", E::unsupported_capability);
        reach = std::max(reach, double(std::abs(v)) + eps);
      }
    }
    const auto cost = program_cost(program, reach);
    need(count <= (o.budget.operations - usage.operations) / (cost + 1),
         "surface tape exceeds operation budget", E::resource_limit);
    const SurfaceOrigin origin{r.tape, program.source.version, program.source.height_min, program.source.height_max};
    terrain_field::SurfaceRuntime runtime(std::move(program));
    SurfaceImage image{r.settings, std::vector<SurfaceTexel>(size_t(count))};
    checked(monitor.report({0, "surface.evaluate"}));
    for (size_t i = 0; i < image.texels.size(); ++i) {
      usage.operations += cost;
      if (!(i % 256)) checked(monitor.checkpoint(usage));
      const auto &point = r.points[i];
      auto &out = image.texels[i]; out.coverage = point.coverage;
      if (!point.coverage) continue;
      const float pos[] = {point.position.x, point.position.y, point.position.z};
      const float nrm[] = {point.normal.x, point.normal.y, point.normal.z};
      terrain_field::SurfaceSourceSample source;
      need(runtime.source_at(pos, nrm, nullptr, point.footprint_metres, source), "surface source evaluation failed", E::backend_failure);
      terrain_field::SurfaceAppearance appearance;
      runtime.appearance_at(pos, nrm, nullptr, appearance, point.footprint_metres);
      // Material conversion follows native face baking: tint, rough bias,
      // wetness, metallic, then thin coating in linear color/squared roughness.
      float color[3];
      for (unsigned c = 0; c < 3; ++c)
        color[c] = source.albedo[c] * appearance.tint[c] * (1-appearance.wetness*(1-terrain_field::kSurfaceWetAlbedoScale));
      out.orm = {source.orm[0], std::clamp(source.orm[1]+appearance.rough_bias, 0.f, 1.f)*(1-appearance.wetness) +
          terrain_field::kSurfaceWetRoughness*appearance.wetness,
          runtime.program().metallic_reg < 0 ? source.orm[2] : appearance.metallic};
      const float coat = appearance.coat_coverage;
      if (coat > 0) {
        for (unsigned c = 0; c < 3; ++c) color[c] = color[c]*(1-coat) + appearance.coat_color[c]*coat;
        out.orm.y = std::sqrt(out.orm.y*out.orm.y*(1-coat) + appearance.coat_roughness*appearance.coat_roughness*coat);
        out.orm.z *= 1-coat;
      }
      out.base_color = {color[0], color[1], color[2]};
      out.height_metres = source.height_m;
      V normal = point.normal;
      if (relief) {
        const V axis = std::abs(normal.x) > .999f ? V{0,0,1} : V{1,0,0};
        const V u = unit(add(axis, mul(normal, -dot(axis,normal)))), v = cross(normal,u);
        const float eps = std::max(.5f*point.footprint_metres, .0001f);
        auto height = [&](V direction, float sign) {
          const auto p = add(point.position, mul(direction, eps*sign));
          const float xyz[] = {p.x,p.y,p.z};
          terrain_field::SurfaceSourceSample sample;
          runtime.source_at(xyz,nrm,nullptr,point.footprint_metres,sample);
          return sample.height_m;
        };
        normal = unit(add(normal, mul(add(mul(u,height(u,1)-height(u,-1)),mul(v,height(v,1)-height(v,-1))),-1/(2*eps))));
      }
      const V tangent{point.tangent.x,point.tangent.y,point.tangent.z};
      const V bitangent = mul(cross(point.normal,tangent),point.tangent.w);
      out.normal = unit(V{dot(normal,tangent),dot(normal,bitangent),dot(normal,point.normal)});
    }
    checked(monitor.checkpoint(usage));
    // A declared relief range is a required feature even if this raster happens
    // to sample zero. Do not advertise such a material as lossless plain PBR.
    need(r.settings.output != SurfaceOutput::pbr_rgba8 || (origin.height_min == 0 && origin.height_max == 0),
         "conventional PBR cannot preserve declared source relief", E::unsupported_capability);
    return finish_surface(job, image, o, monitor, usage, origin);
  });
}
} // namespace matter::asset
