#pragma once
#include "face_material_bake.h"
#include <map>
#include "render/vt_finite_sources.h"
#include "render/vt_types.h"
#include "projected_face_cache.h"
namespace script_host { struct EvaluatedFiniteSurface; struct EvaluatedDirectSurface; }
namespace vt { struct VtPartSnapshot; }
namespace part_surface {
struct MaterialMapping {
    uint32_t module=0;
    gpu_meshing::FaceFrame frame;
    std::array<float,2> phase{},u_range_m{-1e20f,1e20f};
    float datum_m=0;
};
struct Prepared {
    enum class Kind { Finite, Direct };
    Kind kind = Kind::Finite;
    uint64_t part_hash = 0, base_hash = 0, periodic_hash = 0;
    uint32_t material = 0;
    std::string base_program;
    std::shared_ptr<const vt::VtFiniteSources> sources;
    std::vector<std::shared_ptr<const vt::VtPartSnapshot>> modules;
    std::vector<MaterialMapping> material_mappings;
};
using MaterialBaker = std::function<bool(const gpu_meshing::FaceMaterialJob&,
    gpu_meshing::FaceMaterialPatch&, gpu_meshing::FaceStats&, gpu_meshing::Error&,
    const gpu_meshing::BuildControl&)>;
struct SourceCache { std::map<uint64_t,std::weak_ptr<const surface_stamp::Stamp>> faces; };
struct Stats { uint32_t geometry_hits = 0, material_hits = 0, faces = 0; size_t bytes = 0; };
// Worker-owned preparation. Callbacks marshal device work; complete geometry
// cache IO and material projection/filtering remain on the worker. No partial
// catalog escapes. Appearance is deliberately absent from geometry cache keys.
bool prepare(const script_host::EvaluatedFiniteSurface&, const std::string& cache_root,
    const gpu_meshing::SolidFaceProjector&, const MaterialBaker&,
    std::shared_ptr<const Prepared>&, Stats&, gpu_meshing::Error&,
    const gpu_meshing::BuildControl& = {}, SourceCache* = nullptr);
// Direct materials publish an owned GPU recipe; there is no source-image job.
bool prepare_direct(const script_host::EvaluatedDirectSurface&,
    std::shared_ptr<const Prepared>&, gpu_meshing::Error&,
    const gpu_meshing::BuildControl& = {});
// Scratch survives registration (residency takes its own copy). The returned
// context ALSO aliases `prepared`: surface_tape_text and surface_materials point
// into it. The caller must keep the same shared_ptr<const Prepared> alive until
// VtResidency::register_variant / update_variant_surface has returned. Require
// each receiver vertex to match exactly one outward source plane; no texture scale.
struct BindingScratch { std::vector<uint32_t> ids; std::vector<uint8_t> weights; };
bool bind(const Prepared&, vt::VtPartContext&, BindingScratch&, std::string& error);
}
