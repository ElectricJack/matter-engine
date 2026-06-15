#include "particle_culling.h"
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <cassert>

float lattice_vhash(int x, int y, int z) {
    uint32_t h = ((uint32_t)x * 374761393u) ^ ((uint32_t)y * 668265263u) ^ ((uint32_t)z * 2147483647u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFFu; // [0,1]
}

float lattice_vnoise(float x, float y, float z) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    auto lerpf  = [](float a, float b, float t) { return a + (b - a) * t; };
    auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
    float u = smooth(xf), v = smooth(yf), w = smooth(zf);
    float c000 = lattice_vhash(xi, yi, zi),     c100 = lattice_vhash(xi+1, yi, zi);
    float c010 = lattice_vhash(xi, yi+1, zi),   c110 = lattice_vhash(xi+1, yi+1, zi);
    float c001 = lattice_vhash(xi, yi, zi+1),   c101 = lattice_vhash(xi+1, yi, zi+1);
    float c011 = lattice_vhash(xi, yi+1, zi+1), c111 = lattice_vhash(xi+1, yi+1, zi+1);
    float x00 = lerpf(c000, c100, u), x10 = lerpf(c010, c110, u);
    float x01 = lerpf(c001, c101, u), x11 = lerpf(c011, c111, u);
    return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w); // [0,1]
}

bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin) {
    for (int dz = -margin; dz <= margin; ++dz)
    for (int dy = -margin; dy <= margin; ++dy)
    for (int dx = -margin; dx <= margin; ++dx) {
        if (!occ.occupied(SlotCoord{c.x + dx, c.y + dy, c.z + dz})) return false;
    }
    return true;
}

// Build one emitted particle for a slot. Jitter and tint are pure functions of
// (SlotCoord, seed) so the same design always bakes identically.
static EmittedParticle make_particle(const Lattice& lat, SlotCoord c,
                                     const SlotData& d, const CullParams& p) {
    Vector3 base = lat.slot_position(c);
    int s = (int)p.seed;
    float jx = (lattice_vhash(c.x * 2 + 1 + s, c.y, c.z) - 0.5f) * p.jitter_amount;
    float jy = (lattice_vhash(c.x, c.y * 2 + 1 + s, c.z) - 0.5f) * p.jitter_amount;
    float jz = (lattice_vhash(c.x, c.y, c.z * 2 + 1 + s) - 0.5f) * p.jitter_amount;

    EmittedParticle ep;
    ep.position   = Vector3{ base.x + jx, base.y + jy, base.z + jz };
    // Radius variation has two parts: a low-frequency value-noise term shared by
    // neighboring slots (so big and small particles form clumps) plus a small
    // per-particle term that breaks up the clumps. Cluster term dominates.
    float f = p.radius_cluster_freq;
    float cluster = (lattice_vnoise(c.x * f, c.y * f, c.z * f) - 0.5f) * 2.0f; // [-1,1]
    float fine    = (lattice_vhash(c.x + 211 + s, c.y + 211, c.z + 211) - 0.5f) * 2.0f;
    float rv = cluster * 0.75f + fine * 0.25f;
    ep.radius     = p.base_radius * (1.0f + rv * p.radius_variation);
    ep.materialId = d.materialId;
    float tr = lattice_vhash(c.x + 101 + s, c.y, c.z);
    float tg = lattice_vhash(c.x, c.y + 101 + s, c.z);
    float tb = lattice_vhash(c.x, c.y, c.z + 101 + s);
    ep.tint = Vector4{ tr, tg, tb, p.tint_alpha };
    return ep;
}

// Integer cell coordinate of a slot, on the same grid the Cluster keys cells on.
static SlotCoord cell_coord_of(const Lattice& lat, SlotCoord c, const CullParams& p) {
    Vector3 base = lat.slot_position(c);
    int cx = (int)floorf((base.x + p.cell_origin_offset.x) / p.cell_size);
    int cy = (int)floorf((base.y + p.cell_origin_offset.y) / p.cell_size);
    int cz = (int)floorf((base.z + p.cell_origin_offset.z) / p.cell_size);
    return SlotCoord{cx, cy, cz};
}

std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p,
                                           CullStats* stats,
                                           std::vector<SlotCoord>* no_mesh_cells) {
    assert(p.cell_size > 0.0f && "CullParams.cell_size must be positive");
    int margin = p.margin < 1 ? 1 : p.margin;

    // Pass 1: classify each cell. interior[k] == true iff no slot in cell k is
    // non-buried. coord_of[k] remembers the cell's integer coordinate.
    std::unordered_map<uint64_t, bool> interior;
    std::unordered_map<uint64_t, SlotCoord> coord_of;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        SlotCoord cc = cell_coord_of(lattice, c, p);
        uint64_t k = pack_slot(cc);
        bool buried = slot_is_buried(occ, c, margin);
        auto it = interior.find(k);
        if (it == interior.end()) {
            interior.emplace(k, buried);
            coord_of.emplace(k, cc);
        } else if (!buried) {
            it->second = false;
        }
    });

    // Pass 2: core = interior cell whose 26 neighbors are all present + interior.
    static const int OFF[3] = {-1, 0, 1};
    std::unordered_set<uint64_t> core;
    for (const auto& kv : interior) {
        if (!kv.second) continue;
        SlotCoord cc = coord_of[kv.first];
        bool all_in = true;
        for (int dz : OFF) { for (int dy : OFF) { for (int dx : OFF) {
            if (dx == 0 && dy == 0 && dz == 0) continue;
            uint64_t nk = pack_slot(SlotCoord{cc.x + dx, cc.y + dy, cc.z + dz});
            auto it = interior.find(nk);
            if (it == interior.end() || !it->second) { all_in = false; }
        }}}
        if (all_in) core.insert(kv.first);
    }

    // Pass 3: emit every slot whose cell is not core.
    std::vector<EmittedParticle> out;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        uint64_t k = pack_slot(cell_coord_of(lattice, c, p));
        if (core.find(k) == core.end())
            out.push_back(make_particle(lattice, c, d, p));
    });

    if (no_mesh_cells) {
        no_mesh_cells->clear();
        for (const auto& kv : interior)
            if (kv.second) no_mesh_cells->push_back(coord_of[kv.first]);
    }

    if (stats) {
        stats->cells_total = interior.size();
        size_t skipped = 0;
        for (const auto& kv : interior) if (kv.second) ++skipped;
        stats->cells_skipped = skipped;
        stats->cells_core = core.size();
        stats->cells_meshed = stats->cells_total - skipped;
    }
    return out;
}

std::vector<EmittedParticle> emit_all(const Lattice& lattice,
                                      const Occupancy& occ,
                                      const CullParams& p) {
    std::vector<EmittedParticle> out;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        out.push_back(make_particle(lattice, c, d, p));
    });
    return out;
}
