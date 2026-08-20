#pragma once
// tileset_torus_bvh.h — assemble the settled torus into BLAS/TLAS.
//
// Reuses part_asset::load_v2 for baked parts and register_prebuilt to fold
// each part's BLAS entries into a shared BLASManager. The base heightfield
// is tessellated into a single BLAS as instance 0. Every SettledInstance
// becomes an additional TLAS instance whose transform is composed from its
// (px, py, pz) translation, unit-quaternion rotation, and uniform scale.

#include <string>

class BLASManager;
class TLASManager;

namespace tileset {

struct SettledTorus;
struct BakeInputs;

// Fail-closed: false + err on missing/corrupt part file, unnormalized
// quaternion (|q| deviates from 1 by > 1e-3), or empty base grid.
// On success, blas and tlas are populated and `tlas.build(blas)` has been
// called; the managers are in a CPU-ready state. No GPU upload happens here
// (the GL upload path was deleted outright in Phase 5a, tech-debt.md §6).
// The real consumer (render/tileset_bake_vk.cpp) uploads by walking
// BLASManager::get_entries()/TLASManager::get_draw_records() and rebuilds
// unconditionally every bake; BLASManager::content_revision()/
// TLASManager::content_revision() are a landed but currently-unused
// incremental-rebuild signal (tech-debt.md §6), not something a consumer
// reads today.
// Preconditions: `blas` and `tlas` must be EMPTY (the base heightfield is
// registered as TLAS instance 0, and the function verifies the final
// draw-record count against the instances it emitted), and every
// SettledInstance::child_hash must already be baked under
// inputs.parts_cache_dir. On failure both managers are left partially
// populated and should be discarded rather than reused.
//
// Cost: CPU-only and not cheap. It tessellates the whole torus base
// (kTorusN^2 * kSamplesPerTile^2 quads, ~131k triangles at the shipping
// sample count), loads each distinct part off disk once, and builds a SAH TLAS
// over every instance. Bake path only.
bool assemble_torus_bvh(const SettledTorus& settled,
                        const BakeInputs& inputs,
                        BLASManager& blas,
                        TLASManager& tlas,
                        std::string& err);

} // namespace tileset
