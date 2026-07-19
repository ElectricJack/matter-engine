#include "tileset_bake.h"
#include "tileset_layout.h"
#include "tileset_part_collider.h"
#include "tileset_settle.h"
#include "tileset_gtex.h"   // kEngineBakeVersion, kBox3dVersion
#include "part_asset.h"     // fnv1a64

#include <algorithm>        // std::sort (settle_cache_key)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define fs_mkdir_settle(p) _mkdir(p)
#else
#define fs_mkdir_settle(p) ::mkdir(p, 0755)
#endif

namespace tileset {

// ---------------------------------------------------------------------------
// Matrix → Pose extraction (row-major TRS, 4x4)
// Row-major layout: element [r][c] = m[r*4 + c]
// Translation: m[0*4+3]=tx, m[1*4+3]=ty, m[2*4+3]=tz
// Rotation (upper-left 3x3):
//   m[r][c] = m[r*4+c]  so row r, col c
// For a TRS matrix the rotation sub-matrix R is orthonormal.
// We read it as a row-major rotation matrix and convert to quaternion.
// ---------------------------------------------------------------------------
static bool mat_to_pose(const float m[16], Pose& out, std::string& err)
{
    // Extract translation from last column of each row.
    float tx = m[3], ty = m[7], tz = m[11];

    // Extract the 3x3 rotation (rows of m[0..2], columns 0..2).
    // R[i][j] = m[i*4 + j]  (row i, column j)
    float R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            R[i][j] = m[i * 4 + j];

    // Verify orthonormality: each row must be unit length, rows must be orthogonal.
    for (int i = 0; i < 3; ++i) {
        float len2 = R[i][0]*R[i][0] + R[i][1]*R[i][1] + R[i][2]*R[i][2];
        if (std::fabs(len2 - 1.0f) > 1e-3f) {
            err = "tileset_bake: dropChild transform has non-unit rotation row (non-uniform scale not supported)";
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            float dot = R[i][0]*R[j][0] + R[i][1]*R[j][1] + R[i][2]*R[j][2];
            if (std::fabs(dot) > 1e-3f) {
                err = "tileset_bake: dropChild transform rotation rows not orthogonal";
                return false;
            }
        }
    }

    // Standard rotation matrix → quaternion (Shepperd / trace method).
    // The matrix R here is row-major (each R[i] is a row of the rotation matrix).
    // The standard formula uses the matrix as column-major; our R is the transpose
    // of what the formula expects when treating rows as columns.
    // For a rotation matrix M where M[i][j] = R[i][j] (row i, col j):
    //   trace = R[0][0] + R[1][1] + R[2][2]
    float tr = R[0][0] + R[1][1] + R[2][2];
    float qx, qy, qz, qw;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;  // s = 4w
        qw = 0.25f * s;
        qx = (R[2][1] - R[1][2]) / s;
        qy = (R[0][2] - R[2][0]) / s;
        qz = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        float s = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;  // s = 4x
        qw = (R[2][1] - R[1][2]) / s;
        qx = 0.25f * s;
        qy = (R[0][1] + R[1][0]) / s;
        qz = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        float s = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;  // s = 4y
        qw = (R[0][2] - R[2][0]) / s;
        qx = (R[0][1] + R[1][0]) / s;
        qy = 0.25f * s;
        qz = (R[1][2] + R[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;  // s = 4z
        qw = (R[1][0] - R[0][1]) / s;
        qx = (R[0][2] + R[2][0]) / s;
        qy = (R[1][2] + R[2][1]) / s;
        qz = 0.25f * s;
    }

    out.px = tx; out.py = ty; out.pz = tz;
    out.qx = qx; out.qy = qy; out.qz = qz; out.qw = qw;
    return true;
}

// ---------------------------------------------------------------------------
// Sample the tiled base heightfield at world (x, z).
// ---------------------------------------------------------------------------
static float base_height(const BaseField& base, float x, float z, float tile_size)
{
    if (!base.set) return 0.0f;
    const int n = base.n;
    // tile-local coordinate
    float tx = std::fmod(x, tile_size);
    float tz = std::fmod(z, tile_size);
    if (tx < 0.0f) tx += tile_size;
    if (tz < 0.0f) tz += tile_size;
    // grid index (nearest-neighbor; bilinear would also work but spec doesn't mandate it)
    int ix = (int)(tx / base.cell);
    int iz = (int)(tz / base.cell);
    if (ix >= n) ix = n - 1;
    if (iz >= n) iz = n - 1;
    return base.heights[(size_t)iz * n + ix];
}

// ---------------------------------------------------------------------------
// Collider memoization keys
// ---------------------------------------------------------------------------
// Base key: (child_hash, collider_override) — used to cache the unscaled fit.
using ColliderKey = std::pair<uint64_t, std::string>;

// Scaled key: (child_hash, collider_override, scale) — one entry per unique scale.
// Scale derives deterministically from the RNG so exact float comparison is safe.
struct ScaledColliderKey {
    uint64_t    child_hash;
    std::string override_str;
    float       scale;
    bool operator<(const ScaledColliderKey& o) const {
        if (child_hash != o.child_hash) return child_hash < o.child_hash;
        if (override_str != o.override_str) return override_str < o.override_str;
        return scale < o.scale;
    }
};

// ---------------------------------------------------------------------------
// Provenance record for each spawned body
// ---------------------------------------------------------------------------
struct SpawnProv {
    uint64_t child_hash = 0;
    float    scale      = 1.0f;
    int      layer      = -1;   // -1 = drop
};

// ---------------------------------------------------------------------------
// settle_tileset — main orchestrator
// ---------------------------------------------------------------------------

// Samples per tile for the flat-base heightfield (no scripted base() call).
// Mirrors BaseField::kSamplesPerTile for cross-path consistency.
static constexpr int kFlatBaseSamplesPerTile = 8;

bool settle_tileset(const TilesetSpec& spec, const BakeInputs& in,
                    SettledTorus& out, std::string& err)
{
    const float T  = spec.cfg.size;
    const float ET = kTorusN * T;

    // ---- Step 1: build the torus HeightField -----------------------------------
    HeightField hf;
    if (spec.base.set) {
        const int n   = spec.base.n;
        hf.cell       = spec.base.cell;
        hf.count_x    = n * kTorusN + 1;
        hf.count_z    = n * kTorusN + 1;
        hf.heights.resize((size_t)hf.count_x * hf.count_z);
        for (int gz = 0; gz < hf.count_z; ++gz) {
            int tz = gz % n;   // tile-local z sample index (wraps at n)
            for (int gx = 0; gx < hf.count_x; ++gx) {
                int tx = gx % n;
                hf.heights[(size_t)gz * hf.count_x + gx] =
                    spec.base.heights[(size_t)tz * n + tx];
            }
        }
    } else {
        hf.cell    = T / kFlatBaseSamplesPerTile;
        hf.count_x = hf.count_z = (int)(ET / hf.cell) + 1;
        hf.heights.assign((size_t)hf.count_x * hf.count_z, 0.0f);
    }

    // ---- Step 2: memoize colliders -----------------------------------------------
    // Base cache: (child_hash, collider_override) -> unscaled ColliderFit.
    std::map<ColliderKey, ColliderFit> collider_cache;
    // Scaled cache: (child_hash, collider_override, scale) -> scale_fit(base, scale).
    // std::map is pointer-stable so &entry is valid for the lifetime of settle_tileset.
    // Scale 1.0 is stored here too (scale_fit(base, 1.0) == base, but the pointer
    // from the scaled map is used uniformly so BodySpawn.collider always reflects
    // the intended simulation geometry.
    std::map<ScaledColliderKey, ColliderFit> scaled_cache;

    // Helper: get or load a base ColliderFit for a (hash, override) pair.
    // Returns nullptr and sets err on failure.
    auto get_base_collider = [&](uint64_t hash, const std::string& override_str,
                                 const std::string& layer_module) -> const ColliderFit* {
        ColliderKey key{ hash, override_str };
        auto it = collider_cache.find(key);
        if (it != collider_cache.end()) return &it->second;

        const char* ov = override_str.empty() ? nullptr : override_str.c_str();
        ColliderFit fit;
        std::string cerr;
        if (!collider_for_part(in.parts_cache_dir, hash, ov, fit, cerr)) {
            std::ostringstream ss;
            ss << "settle_tileset: layer \"" << layer_module
               << "\" collider_for_part failed for hash 0x" << std::hex << hash
               << ": " << cerr;
            err = ss.str();
            return nullptr;
        }
        collider_cache[key] = std::move(fit);
        return &collider_cache[key];
    };

    // Helper: get or create a scaled ColliderFit for (hash, override, scale).
    // Depends on get_base_collider, so defined after it.
    // For drops (scale always 1.0) and physics placements with p.scale != 1.0 alike,
    // BodySpawn.collider always points into scaled_cache so the simulated geometry
    // matches the intended scale.
    auto get_scaled_collider = [&](uint64_t hash, const std::string& override_str,
                                   float scale,
                                   const std::string& layer_module) -> const ColliderFit* {
        const ColliderFit* base = get_base_collider(hash, override_str, layer_module);
        if (!base) return nullptr;
        ScaledColliderKey sk{ hash, override_str, scale };
        auto it = scaled_cache.find(sk);
        if (it != scaled_cache.end()) return &it->second;
        scaled_cache[sk] = scale_fit(*base, scale);
        return &scaled_cache[sk];
    };

    // Pre-load colliders for drops.
    for (const auto& dr : spec.drops) {
        if (!get_base_collider(dr.child_hash, "", "drop")) return false;
    }
    // Pre-load colliders for all layers.
    for (const auto& ls : spec.layers) {
        const std::string& ov = ls.collider_override;
        auto check_placement = [&](const Placement& p) -> bool {
            return get_base_collider(p.child_hash, ov, ls.module) != nullptr;
        };
        for (int o = 0; o < 2; ++o)
            for (int c = 0; c < 2; ++c)
                for (const auto& p : ls.strip[o][c])
                    if (!check_placement(p)) return false;
        for (int t = 0; t < 16; ++t)
            for (const auto& p : ls.interior[t])
                if (!check_placement(p)) return false;
    }

    // ---- Step 3: create SettleWorld ---------------------------------------------
    SettleWorld world(ET, hf, SettleParams{});

    // ---- Step 4: shared drops ---------------------------------------------------
    // One sync group per DropChildRec with 16 occurrence frames (one per tile).
    // The drop transform's translation/rotation goes into the SPAWN pose.
    // Frames are pure translations: {col*T, 0, row*T}.

    std::vector<BodySpawn> drop_spawns;
    std::vector<SpawnProv> drop_provs;

    for (const auto& dr : spec.drops) {
        // Parse the transform matrix into a spawn pose.
        Pose spawn_pose;
        if (!mat_to_pose(dr.transform, spawn_pose, err)) return false;

        // 16 occurrence frames: one per tile (row r, col c).
        std::vector<Pose> frames;
        frames.reserve(16);
        for (int r = 0; r < kTorusN; ++r)
            for (int c = 0; c < kTorusN; ++c)
                frames.push_back(Pose{ c * T, 0.0f, r * T, 0.0f, 0.0f, 0.0f, 1.0f });

        int sg = world.add_sync_group(frames);

        // Drops always have scale 1.0 (DropChildRec carries no scale field).
        // Use the scaled cache (scale=1.0) so the pointer convention is uniform.
        const ColliderFit* scaled_fit = get_scaled_collider(dr.child_hash, "", 1.0f, "drop");

        // One spawn per occurrence frame.
        for (int k = 0; k < 16; ++k) {
            BodySpawn bs;
            bs.collider   = scaled_fit;
            bs.start      = spawn_pose;
            bs.sync_group = sg;
            bs.instance   = k;
            drop_spawns.push_back(bs);

            SpawnProv pv;
            pv.child_hash = dr.child_hash;
            pv.scale      = 1.0f;
            pv.layer      = -1;
            drop_provs.push_back(pv);
        }
    }

    // Settle all drops in one batch.
    LayerResult drop_result;
    if (!drop_spawns.empty()) {
        drop_result = world.settle_layer(drop_spawns);
        if (!drop_result.converged) out.report.converged_all = false;
    }

    // ---- Step 5: per script layer -----------------------------------------------
    struct NonPhysInst {
        uint64_t child_hash;
        float    scale;
        Pose     pose;
        int      layer;
    };

    // Provenance for physics spawns (one per layer).
    std::vector<std::vector<SpawnProv>> layer_phys_provs;
    // Non-physics instances appended after each layer.
    std::vector<NonPhysInst> all_nonphys;

    int layer_idx = 0;
    for (const auto& ls : spec.layers) {
        const std::string& ov = ls.collider_override;
        std::vector<BodySpawn> layer_spawns;
        std::vector<SpawnProv> layer_provs;
        std::vector<NonPhysInst> layer_nonphys;

        if (ls.physics) {
            // --- Strip placements (physics) ---
            for (int o = 0; o < 2; ++o) {
                for (int c = 0; c < 2; ++c) {
                    const auto& strip_vec = ls.strip[o][c];
                    for (size_t pi = 0; pi < strip_vec.size(); ++pi) {
                        const Placement& p = strip_vec[pi];
                        bool is_vertical = (o == 0);

                        // 8 occurrence frames from strip_occurrences.
                        auto occs = strip_occurrences(c, is_vertical);
                        // occs.size() should be 8.

                        std::vector<Pose> frames;
                        frames.reserve(occs.size());
                        for (const auto& oc : occs) {
                            Pose frame;
                            if (is_vertical) {
                                // vertical strip: seam runs along Z, perpendicular is X
                                // boundary at x = oc.boundary * T; lane offset z = oc.lane * T
                                frame.px = (float)oc.boundary * T;
                                frame.py = 0.0f;
                                frame.pz = (float)oc.lane * T;
                            } else {
                                // horizontal strip: seam runs along X, perpendicular is Z
                                // boundary at z = oc.boundary * T; lane offset x = oc.lane * T
                                frame.px = (float)oc.lane * T;
                                frame.py = 0.0f;
                                frame.pz = (float)oc.boundary * T;
                            }
                            frame.qx = 0.0f; frame.qy = 0.0f;
                            frame.qz = 0.0f; frame.qw = 1.0f;
                            frames.push_back(frame);
                        }

                        int sg = world.add_sync_group(frames);

                        // Scaled collider: use p.scale so physics simulates with the
                        // correct geometry even when scale != 1.0.
                        const ColliderFit* scaled_fit =
                            get_scaled_collider(p.child_hash, ov, p.scale, ls.module);
                        if (!scaled_fit) return false;

                        // Spawn pose (strip-local): pos from placement, rotation from placement.
                        //
                        // Axis convention (verified against dsl_bindings.cpp j_ts_layer):
                        //   DSL records vertical:   p.pos = {across, y, along}
                        //   DSL records horizontal: p.pos = {along,  y, across}  (axes swapped at recording time)
                        //   Frame construction here: vertical   frame = {boundary*T, 0, lane*T}
                        //                            horizontal frame = {lane*T,     0, boundary*T}
                        //   Both swaps compose so world = frame + spawn_pose gives:
                        //     vertical   world_x = boundary*T + across,  world_z = lane*T    + along
                        //     horizontal world_x = lane*T     + along,   world_z = boundary*T + across
                        //   The non-physics snap path (below) uses the identical formula, confirming
                        //   that direct assignment of p.pos[0]/p.pos[2] is correct for both orientations.
                        Pose spawn_pose;
                        spawn_pose.px = p.pos[0];
                        spawn_pose.py = p.pos[1];
                        spawn_pose.pz = p.pos[2];
                        spawn_pose.qx = p.quat[0];
                        spawn_pose.qy = p.quat[1];
                        spawn_pose.qz = p.quat[2];
                        spawn_pose.qw = p.quat[3];

                        // One spawn per occurrence.
                        for (int k = 0; k < (int)occs.size(); ++k) {
                            BodySpawn bs;
                            bs.collider   = scaled_fit;
                            bs.start      = spawn_pose;
                            bs.sync_group = sg;
                            bs.instance   = k;
                            layer_spawns.push_back(bs);

                            SpawnProv pv;
                            pv.child_hash = p.child_hash;
                            pv.scale      = p.scale;
                            pv.layer      = layer_idx;
                            layer_provs.push_back(pv);
                        }
                    }
                }
            }

            // --- Interior placements (physics) ---
            for (int t = 0; t < 16; ++t) {
                int row = t / kTorusN;
                int col = t % kTorusN;
                for (const auto& p : ls.interior[t]) {
                    // Scaled collider: use p.scale so physics simulates with the
                    // correct geometry even when scale != 1.0.
                    const ColliderFit* scaled_fit =
                        get_scaled_collider(p.child_hash, ov, p.scale, ls.module);
                    if (!scaled_fit) return false;

                    float wx = (float)col * T + p.pos[0];
                    float wy = p.pos[1];
                    float wz = (float)row * T + p.pos[2];

                    BodySpawn bs;
                    bs.collider   = scaled_fit;
                    bs.start      = Pose{ wx, wy, wz, p.quat[0], p.quat[1], p.quat[2], p.quat[3] };
                    bs.sync_group = -1;
                    layer_spawns.push_back(bs);

                    SpawnProv pv;
                    pv.child_hash = p.child_hash;
                    pv.scale      = p.scale;
                    pv.layer      = layer_idx;
                    layer_provs.push_back(pv);
                }
            }

        } else {
            // Non-physics: analytically snap, no physics spawn.
            // Process all placements (interior only per spec; strips with physics:false
            // would also be snapped but are unusual).
            for (int o = 0; o < 2; ++o) {
                for (int c = 0; c < 2; ++c) {
                    for (const auto& p : ls.strip[o][c]) {
                        const ColliderFit* base_fit = get_base_collider(p.child_hash, ov, ls.module);
                        if (!base_fit) return false;
                        ColliderFit scaled = scale_fit(*base_fit, p.scale);
                        float fh = fit_half_height(scaled);
                        auto occs = strip_occurrences(c, o == 0);
                        for (const auto& oc : occs) {
                            float wx, wz;
                            if (o == 0) { // vertical
                                wx = (float)oc.boundary * T + p.pos[0];
                                wz = (float)oc.lane    * T + p.pos[2];
                            } else { // horizontal
                                wx = (float)oc.lane    * T + p.pos[0];
                                wz = (float)oc.boundary * T + p.pos[2];
                            }
                            float h  = base_height(spec.base, wx, wz, T);
                            float wy = h + fh - ls.embed * 2.0f * fh;
                            NonPhysInst ni;
                            ni.child_hash = p.child_hash;
                            ni.scale      = p.scale;
                            ni.pose       = Pose{ wx, wy, wz, p.quat[0], p.quat[1], p.quat[2], p.quat[3] };
                            ni.layer      = layer_idx;
                            layer_nonphys.push_back(ni);
                        }
                    }
                }
            }
            for (int t = 0; t < 16; ++t) {
                int row = t / kTorusN;
                int col = t % kTorusN;
                for (const auto& p : ls.interior[t]) {
                    const ColliderFit* base_fit = get_base_collider(p.child_hash, ov, ls.module);
                    if (!base_fit) return false;
                    ColliderFit scaled = scale_fit(*base_fit, p.scale);
                    float fh = fit_half_height(scaled);

                    float wx = (float)col * T + p.pos[0];
                    float wz = (float)row * T + p.pos[2];
                    float h  = base_height(spec.base, wx, wz, T);
                    float wy = h + fh - ls.embed * 2.0f * fh;

                    NonPhysInst ni;
                    ni.child_hash = p.child_hash;
                    ni.scale      = p.scale;
                    ni.pose       = Pose{ wx, wy, wz, p.quat[0], p.quat[1], p.quat[2], p.quat[3] };
                    ni.layer      = layer_idx;
                    layer_nonphys.push_back(ni);
                }
            }
        }

        // Settle this layer's physics bodies.
        if (!layer_spawns.empty()) {
            LayerResult lr = world.settle_layer(layer_spawns);
            out.report.layers.push_back(lr);
            if (!lr.converged) out.report.converged_all = false;
        } else {
            // Non-physics-only layer: push a trivial result.
            LayerResult lr;
            lr.converged = true;
            out.report.layers.push_back(lr);
        }

        layer_phys_provs.push_back(std::move(layer_provs));
        for (auto& ni : layer_nonphys)
            all_nonphys.push_back(ni);

        ++layer_idx;
    }

    // ---- Step 6: finalize and read back poses -----------------------------------
    world.finalize();
    const std::vector<Pose>& phys_poses = world.poses();
    out.report.pose_hash = world.pose_hash();

    // ---- Step 7: assemble output instances in the correct order -----------------
    // Order: drops first, then per layer: physics instances then non-physics.

    size_t pose_idx = 0;

    // Drop instances (all drops were settle_layer'd as a single batch before any layer).
    for (size_t i = 0; i < drop_provs.size(); ++i) {
        SettledInstance si;
        si.child_hash = drop_provs[i].child_hash;
        si.scale      = drop_provs[i].scale;
        si.pose       = phys_poses[pose_idx++];
        si.layer      = -1;
        out.instances.push_back(si);
    }

    // Per-layer physics instances.
    for (size_t li = 0; li < layer_phys_provs.size(); ++li) {
        for (const auto& pv : layer_phys_provs[li]) {
            SettledInstance si;
            si.child_hash = pv.child_hash;
            si.scale      = pv.scale;
            si.pose       = phys_poses[pose_idx++];
            si.layer      = pv.layer;
            out.instances.push_back(si);
        }
        // Non-physics instances for this layer (in all_nonphys with matching layer).
        // They were accumulated in placement order.
        for (const auto& ni : all_nonphys) {
            if (ni.layer == (int)li) {
                SettledInstance si;
                si.child_hash = ni.child_hash;
                si.scale      = ni.scale;
                si.pose       = ni.pose;
                si.layer      = ni.layer;
                out.instances.push_back(si);
            }
        }
    }

    // ---- Step 8: fill remaining output fields -----------------------------------
    out.cfg             = spec.cfg;
    out.base            = spec.base;
    out.variant_ranges  = spec.variant_ranges;

    // report.layers already populated per-layer above; converged_all set throughout.

    return true;
}

// ---------------------------------------------------------------------------
// Settle-result cache — plain little-endian binary serialization.
//
// File header (fixed):
//   uint32_t magic    = 0x544C5343  ('CSTL' = Cache SeTtLe)
//   uint32_t version  = 1
//   uint64_t key                 (lookup key written at save time)
//   uint32_t engine_bake_version (from kEngineBakeVersion)
//   uint32_t box3d_version       (from kBox3dVersion)
//
// Body (all little-endian):
//   TileConfig cfg               (5 floats + 1 uint64 + 2 floats = fixed layout)
//   BaseField  base              (int n, float cell, uint32 material, bool set, N*float heights)
//   uint32_t   instance_count
//   SettledInstance[instance_count] (uint64 child_hash, float scale, Pose[7 floats], int layer)
//   uint32_t   variant_range_count
//   VariantRange[variant_range_count] (int tile, 4*size_t)
//   bool       converged_all
//   uint64_t   pose_hash
//   uint32_t   layer_count
//   LayerResult[layer_count] (bool converged, int awake_count, float sim_time)
// ---------------------------------------------------------------------------

static constexpr uint32_t kSettleCacheMagic   = 0x434C5453u; // 'STLC' little-endian
static constexpr uint32_t kSettleCacheVersion = 1u;

// Helpers: write/read primitive types to/from a file stream in little-endian.
namespace {
template<typename T>
static bool write_le(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
    return f.good();
}
template<typename T>
static bool read_le(std::ifstream& f, T& v) {
    f.read(reinterpret_cast<char*>(&v), sizeof(T));
    return f.good();
}
} // anonymous namespace

uint64_t settle_cache_key(uint64_t script_source_hash,
                          const std::vector<uint64_t>& sorted_child_hashes)
{
    return settle_cache_key(script_source_hash, sorted_child_hashes, "{}");
}

uint64_t settle_cache_key(uint64_t script_source_hash,
                          const std::vector<uint64_t>& sorted_child_hashes,
                          const std::string& canonical_root_params_json)
{
    // FNV-1a over (script_source_hash u64 LE, sorted child hashes u64s LE,
    //              kEngineBakeVersion u32 LE, kBox3dVersion u32 LE).
    std::vector<uint8_t> buf;
    buf.resize(sizeof(uint64_t)
             + sorted_child_hashes.size() * sizeof(uint64_t)
             + canonical_root_params_json.size() + 1
             + sizeof(uint32_t) * 2);
    size_t off = 0;
    std::memcpy(buf.data() + off, &script_source_hash, sizeof(uint64_t));
    off += sizeof(uint64_t);
    for (uint64_t h : sorted_child_hashes) {
        std::memcpy(buf.data() + off, &h, sizeof(uint64_t));
        off += sizeof(uint64_t);
    }
    std::memcpy(buf.data() + off, canonical_root_params_json.data(),
                canonical_root_params_json.size());
    off += canonical_root_params_json.size();
    buf[off++] = 0;
    uint32_t ebv = kEngineBakeVersion;
    uint32_t bv  = kBox3dVersion;
    std::memcpy(buf.data() + off, &ebv, sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(buf.data() + off, &bv,  sizeof(uint32_t));
    return part_asset::fnv1a64(buf.data(), buf.size());
}

static std::string settle_cache_path(const std::string& cache_root, uint64_t key) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.settle", (unsigned long long)key);
    return cache_root + "/tileset/" + buf;
}

bool settle_cache_save(const std::string& cache_root, uint64_t key, const SettledTorus& s) {
    // Ensure tileset subdir exists.
    const std::string tileset_dir = cache_root + "/tileset";
    fs_mkdir_settle(tileset_dir.c_str());

    const std::string path = settle_cache_path(cache_root, key);
    const std::string tmp  = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    // Header.
    if (!write_le(f, kSettleCacheMagic))    return false;
    if (!write_le(f, kSettleCacheVersion))  return false;
    if (!write_le(f, key))                  return false;
    uint32_t ebv = kEngineBakeVersion, bv = kBox3dVersion;
    if (!write_le(f, ebv)) return false;
    if (!write_le(f, bv))  return false;

    // TileConfig: size, texels_per_meter, seed, edge_strip_width, corner_clear_radius.
    if (!write_le(f, s.cfg.size))                 return false;
    if (!write_le(f, (int32_t)s.cfg.texels_per_meter)) return false;
    if (!write_le(f, s.cfg.seed))                 return false;
    if (!write_le(f, s.cfg.edge_strip_width))     return false;
    if (!write_le(f, s.cfg.corner_clear_radius))  return false;

    // BaseField.
    if (!write_le(f, (int32_t)s.base.n))          return false;
    if (!write_le(f, s.base.cell))                 return false;
    if (!write_le(f, s.base.material))             return false;
    uint8_t base_set = s.base.set ? 1 : 0;
    if (!write_le(f, base_set))                    return false;
    // Heights: n*n floats.
    if (s.base.set && s.base.n > 0) {
        size_t hn = (size_t)s.base.n * s.base.n;
        f.write(reinterpret_cast<const char*>(s.base.heights.data()),
                (std::streamsize)(hn * sizeof(float)));
        if (!f.good()) return false;
    }

    // Instances.
    uint32_t inst_count = (uint32_t)s.instances.size();
    if (!write_le(f, inst_count)) return false;
    for (const auto& si : s.instances) {
        if (!write_le(f, si.child_hash)) return false;
        if (!write_le(f, si.scale))      return false;
        if (!write_le(f, si.pose.px))    return false;
        if (!write_le(f, si.pose.py))    return false;
        if (!write_le(f, si.pose.pz))    return false;
        if (!write_le(f, si.pose.qx))    return false;
        if (!write_le(f, si.pose.qy))    return false;
        if (!write_le(f, si.pose.qz))    return false;
        if (!write_le(f, si.pose.qw))    return false;
        if (!write_le(f, (int32_t)si.layer)) return false;
    }

    // VariantRanges.
    uint32_t vr_count = (uint32_t)s.variant_ranges.size();
    if (!write_le(f, vr_count)) return false;
    for (const auto& vr : s.variant_ranges) {
        if (!write_le(f, (int32_t)vr.tile))     return false;
        if (!write_le(f, (uint64_t)vr.op_begin))    return false;
        if (!write_le(f, (uint64_t)vr.op_end))      return false;
        if (!write_le(f, (uint64_t)vr.child_begin)) return false;
        if (!write_le(f, (uint64_t)vr.child_end))   return false;
    }

    // Report.
    uint8_t conv_all = s.report.converged_all ? 1 : 0;
    if (!write_le(f, conv_all))             return false;
    if (!write_le(f, s.report.pose_hash))   return false;
    uint32_t layer_count = (uint32_t)s.report.layers.size();
    if (!write_le(f, layer_count)) return false;
    for (const auto& lr : s.report.layers) {
        uint8_t lconv = lr.converged ? 1 : 0;
        if (!write_le(f, lconv))             return false;
        if (!write_le(f, (int32_t)lr.awake_count)) return false;
        if (!write_le(f, lr.sim_time))       return false;
    }

    f.close();
    if (!f.good() && !f.eof()) return false;
    // Atomic rename.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool settle_cache_load(const std::string& cache_root, uint64_t key, SettledTorus& out) {
    const std::string path = settle_cache_path(cache_root, key);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Measure file size upfront so counts can be validated before resize.
    // Fail-closed: any inconsistency between an on-disk count and remaining bytes
    // returns false (treat as miss); no exceptions escape, no crash on truncation.
    f.seekg(0, std::ios::end);
    if (!f.good()) return false;
    const std::streamoff file_size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (!f.good() || file_size < 0) return false;

    // Returns remaining bytes from current stream position.
    auto remaining = [&]() -> size_t {
        std::streamoff pos = f.tellg();
        if (pos < 0 || pos > file_size) return 0;
        return static_cast<size_t>(file_size - pos);
    };

    // Header validation.
    uint32_t magic = 0, version = 0;
    uint64_t stored_key = 0;
    uint32_t ebv = 0, bv = 0;
    if (!read_le(f, magic))      return false;
    if (magic != kSettleCacheMagic) return false;
    if (!read_le(f, version))    return false;
    if (version != kSettleCacheVersion) return false;
    if (!read_le(f, stored_key)) return false;
    if (stored_key != key)       return false;  // key mismatch → treat as miss
    if (!read_le(f, ebv))        return false;
    if (ebv != kEngineBakeVersion) return false; // version bump → reject
    if (!read_le(f, bv))         return false;
    if (bv != kBox3dVersion)     return false;

    // TileConfig.
    if (!read_le(f, out.cfg.size))                 return false;
    int32_t tpm = 0;
    if (!read_le(f, tpm)) return false;
    out.cfg.texels_per_meter = (int)tpm;
    if (!read_le(f, out.cfg.seed))                 return false;
    if (!read_le(f, out.cfg.edge_strip_width))     return false;
    if (!read_le(f, out.cfg.corner_clear_radius))  return false;

    // BaseField. bn must be 0 or kSamplesPerTile; other values are corrupt.
    int32_t bn = 0;
    if (!read_le(f, bn))              return false;
    if (bn != 0 && bn != BaseField::kSamplesPerTile) return false;
    out.base.n = (int)bn;
    if (!read_le(f, out.base.cell))   return false;
    if (!read_le(f, out.base.material)) return false;
    uint8_t base_set = 0;
    if (!read_le(f, base_set))        return false;
    out.base.set = (base_set != 0);
    if (out.base.set && out.base.n > 0) {
        size_t hn = (size_t)out.base.n * out.base.n;
        // Validate: hn floats must fit in the remaining file bytes.
        if (hn > remaining() / sizeof(float)) return false;
        out.base.heights.resize(hn);
        f.read(reinterpret_cast<char*>(out.base.heights.data()),
               (std::streamsize)(hn * sizeof(float)));
        if (!f.good()) return false;
    }

    // Instances. Each SettledInstance on disk: uint64 + float + 7*float + int32 = 44 bytes.
    static constexpr size_t kInstanceBytes = sizeof(uint64_t) + 8 * sizeof(float) + sizeof(int32_t);
    uint32_t inst_count = 0;
    if (!read_le(f, inst_count)) return false;
    if (inst_count > (1u << 20)) return false;  // > 1M instances → corrupt
    if ((size_t)inst_count * kInstanceBytes > remaining()) return false;
    out.instances.resize(inst_count);
    for (auto& si : out.instances) {
        if (!read_le(f, si.child_hash)) return false;
        if (!read_le(f, si.scale))      return false;
        if (!read_le(f, si.pose.px))    return false;
        if (!read_le(f, si.pose.py))    return false;
        if (!read_le(f, si.pose.pz))    return false;
        if (!read_le(f, si.pose.qx))    return false;
        if (!read_le(f, si.pose.qy))    return false;
        if (!read_le(f, si.pose.qz))    return false;
        if (!read_le(f, si.pose.qw))    return false;
        int32_t layer = 0;
        if (!read_le(f, layer)) return false;
        si.layer = (int)layer;
    }

    // VariantRanges. Each on disk: int32 + 4*uint64 = 36 bytes.
    static constexpr size_t kVRBytes = sizeof(int32_t) + 4 * sizeof(uint64_t);
    uint32_t vr_count = 0;
    if (!read_le(f, vr_count)) return false;
    if (vr_count > (1u << 20)) return false;  // > 1M ranges → corrupt
    if ((size_t)vr_count * kVRBytes > remaining()) return false;
    out.variant_ranges.resize(vr_count);
    for (auto& vr : out.variant_ranges) {
        int32_t tile = 0;
        if (!read_le(f, tile)) return false;
        vr.tile = (int)tile;
        uint64_t op_begin = 0, op_end = 0, child_begin = 0, child_end = 0;
        if (!read_le(f, op_begin))    return false;
        if (!read_le(f, op_end))      return false;
        if (!read_le(f, child_begin)) return false;
        if (!read_le(f, child_end))   return false;
        vr.op_begin    = (size_t)op_begin;
        vr.op_end      = (size_t)op_end;
        vr.child_begin = (size_t)child_begin;
        vr.child_end   = (size_t)child_end;
    }

    // Report. Each LayerResult on disk: uint8 + int32 + float = 9 bytes.
    static constexpr size_t kLayerBytes = sizeof(uint8_t) + sizeof(int32_t) + sizeof(float);
    uint8_t conv_all = 0;
    if (!read_le(f, conv_all))                return false;
    out.report.converged_all = (conv_all != 0);
    if (!read_le(f, out.report.pose_hash))    return false;
    uint32_t layer_count = 0;
    if (!read_le(f, layer_count)) return false;
    if (layer_count > (1u << 10)) return false;  // > 1024 layers → corrupt
    if ((size_t)layer_count * kLayerBytes > remaining()) return false;
    out.report.layers.resize(layer_count);
    for (auto& lr : out.report.layers) {
        uint8_t lconv = 0;
        if (!read_le(f, lconv)) return false;
        lr.converged = (lconv != 0);
        int32_t awake = 0;
        if (!read_le(f, awake)) return false;
        lr.awake_count = (int)awake;
        if (!read_le(f, lr.sim_time)) return false;
    }

    return true;
}

} // namespace tileset
