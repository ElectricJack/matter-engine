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

// 4-octave fractional Brownian motion over the value-noise field. ~[0,1).
static float fbm3(float x, float y, float z) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int i = 0; i < 4; ++i) {
        sum += amp * lattice_vnoise(x * freq, y * freq, z * freq);
        freq *= 2.0f; amp *= 0.5f;
    }
    return sum;
}

bool slot_is_buried(const Occupancy& occ, SlotCoord c, int margin) {
    for (int dz = -margin; dz <= margin; ++dz)
    for (int dy = -margin; dy <= margin; ++dy)
    for (int dx = -margin; dx <= margin; ++dx) {
        if (!occ.occupied(SlotCoord{c.x + dx, c.y + dy, c.z + dz})) return false;
    }
    return true;
}

int slot_depth(const Occupancy& occ, SlotCoord c, int max_depth) {
    if (max_depth < 0) max_depth = 0;
    int k = 0;
    for (; k < max_depth; ++k) {
        int r = k + 1;  // does the radius-(k+1) box stay fully occupied?
        bool full = true;
        for (int dz = -r; dz <= r && full; ++dz)
        for (int dy = -r; dy <= r && full; ++dy)
        for (int dx = -r; dx <= r && full; ++dx) {
            if (!occ.occupied(SlotCoord{c.x + dx, c.y + dy, c.z + dz})) full = false;
        }
        if (!full) break;
    }
    return k;
}

int slot_tier(int depth, int max_tier) {
    if (max_tier < 0) max_tier = 0;
    int d = depth < 0 ? 0 : depth;
    if (d > max_tier) d = max_tier;
    return max_tier - d;
}

// Build one emitted sub-particle for tier `tier` at sub-offset (ox,oy,oz) within
// slot `c`. Tier 0 with offset (0,0,0) reproduces the legacy one-particle-per-
// slot output exactly. Continuous fields (radius clusters, marble veins) sample
// the centered fractional lattice coord; per-particle uniqueness hashes the fine
// integer coord. All deterministic in (SlotCoord, sub-offset, seed).
static EmittedParticle make_sub_particle(const Lattice& lat, SlotCoord c, int tier,
                                         int ox, int oy, int oz,
                                         const SlotData& d, const CullParams& p) {
    int   scale = 1 << tier;                 // 2^tier
    float inv   = 1.0f / (float)scale;
    int s = (int)p.seed;

    // Fine integer coord (per-particle hashing) and centered fractional coord
    // (continuous noise). Centered: fr == 0 at tier 0, so legacy output is exact.
    int   fx = c.x * scale + ox, fy = c.y * scale + oy, fz = c.z * scale + oz;
    float frx = (ox + 0.5f) * inv - 0.5f;
    float fry = (oy + 0.5f) * inv - 0.5f;
    float frz = (oz + 0.5f) * inv - 0.5f;
    float cfx = c.x + frx, cfy = c.y + fry, cfz = c.z + frz;

    Vector3 base = lat.slot_position(c);
    float spacing = p.spacing;
    float jamt = p.jitter_amount * inv;       // jitter proportional to sub-spacing
    float jx = (lattice_vhash(fx * 2 + 1 + s, fy, fz) - 0.5f) * jamt;
    float jy = (lattice_vhash(fx, fy * 2 + 1 + s, fz) - 0.5f) * jamt;
    float jz = (lattice_vhash(fx, fy, fz * 2 + 1 + s) - 0.5f) * jamt;

    EmittedParticle ep;
    ep.position = Vector3{ base.x + frx * spacing + jx,
                          base.y + fry * spacing + jy,
                          base.z + frz * spacing + jz };

    float f = p.radius_cluster_freq;
    float cluster = (lattice_vnoise(cfx * f, cfy * f, cfz * f) - 0.5f) * 2.0f; // [-1,1]
    float fine    = (lattice_vhash(fx + 211 + s, fy + 211, fz + 211) - 0.5f) * 2.0f;
    float rv = cluster * 0.75f + fine * 0.25f;
    ep.radius     = (p.base_radius * inv) * (1.0f + rv * p.radius_variation);
    ep.materialId = d.materialId;
    ep.detail_size = spacing * inv;           // S / 2^tier

    if (p.vein_freq > 0.0f) {
        float turb  = fbm3(cfx * 0.08f + s, cfy * 0.08f, cfz * 0.08f);
        float band  = sinf((cfx + cfy * 0.6f + cfz) * p.vein_freq
                           + p.vein_warp * turb * 6.2831853f);
        float vein  = powf(0.5f + 0.5f * band, 6.0f);
        float mottle = (fbm3(cfx * 0.2f + 50.0f, cfy * 0.2f, cfz * 0.2f) - 0.5f) * 0.10f;
        float L = (0.92f - 0.55f * vein) + mottle;
        if (L < 0.05f) L = 0.05f;
        if (L > 1.0f)  L = 1.0f;
        ep.tint = Vector4{ L, L * 0.97f, L * 0.92f, p.tint_alpha };
    } else {
        float tr = lattice_vhash(fx + 101 + s, fy, fz);
        float tg = lattice_vhash(fx, fy + 101 + s, fz);
        float tb = lattice_vhash(fx, fy, fz + 101 + s);
        ep.tint = Vector4{ tr, tg, tb, p.tint_alpha };
    }
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
        if (core.find(k) != core.end()) return;     // core slots are dropped
        int tier  = slot_tier(slot_depth(occ, c, p.max_tier), p.max_tier);
        int scale = 1 << tier;
        for (int oz = 0; oz < scale; ++oz)
        for (int oy = 0; oy < scale; ++oy)
        for (int ox = 0; ox < scale; ++ox)
            out.push_back(make_sub_particle(lattice, c, tier, ox, oy, oz, d, p));
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
        int tier  = slot_tier(slot_depth(occ, c, p.max_tier), p.max_tier);
        int scale = 1 << tier;
        for (int oz = 0; oz < scale; ++oz)
        for (int oy = 0; oy < scale; ++oy)
        for (int ox = 0; ox < scale; ++ox)
            out.push_back(make_sub_particle(lattice, c, tier, ox, oy, oz, d, p));
    });
    return out;
}
