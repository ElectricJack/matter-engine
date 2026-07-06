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
// called; the managers are in a GPU-upload-ready state.
bool assemble_torus_bvh(const SettledTorus& settled,
                        const BakeInputs& inputs,
                        BLASManager& blas,
                        TLASManager& tlas,
                        std::string& err);

} // namespace tileset
