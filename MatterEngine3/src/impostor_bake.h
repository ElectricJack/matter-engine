#pragma once

// impostor_bake — the terminal representation of a part (redesign §2, §3.3).
//
// A part's LOD ladder stops when decimation stops buying a real reduction
// (M1.5, FlattenTargets::min_level_benefit). Past that point more mesh rungs
// are provably wasted work, so the ladder gets ONE more rung that is not a
// mesh: a two-triangle camera-facing billboard sampling a pre-baked view
// atlas. It is rep N of the SAME distance table — the cull shader's existing
// walk selects it with no new selection code, it rides the existing indirect
// draw, and the LOD trace sees it as just another rung.
//
// ---------------------------------------------------------------------------
// ELIGIBILITY — why kMinTerminalTris = 16
//
// The billboard is 2 triangles and a texture fetch. The mesh ladder admits a
// rung on a 30 % triangle reduction (min_level_benefit); the impostor cannot
// use that floor, because unlike a mesh rung it carries a per-part ATLAS whose
// bytes have no equivalent in the ladder. So it is held to a stricter bar: an
// order-of-magnitude reduction. 2 / N <= 0.125  <=>  N >= 16.
//
// Measured on RockGallery after M1.5, terminal mesh rungs are
//   2, 18, 18, 20, 22, 26, 26, 28, 32, 32, 34, 96
// so the floor excludes exactly the degenerate 2-triangle part (whose
// "impostor" would ADD geometry) and admits everything else at 9x-48x. It is
// also the right shape of rule: it is a property of the part, evaluated per
// cluster at bake time, and it self-terminates without an author guess.
//
// The honest counter-measurement, recorded because it bounds where this tier
// belongs: at 128 KiB per cluster the atlas costs far more BYTES than the mesh
// it replaces (a 20-triangle rung is ~2 KiB of vertex stream). The impostor is
// a per-PART fixed cost amortised over every INSTANCE drawn, so it pays in a
// scatter world and loses in a 13-instance gallery. RockGallery is where it is
// verified, not where it profits.
//
// ---------------------------------------------------------------------------
// ATLAS SIZING — why 16 views of 32x32
//
// VIEWS. Using the nearest baked view instead of the exact one rotates the
// depicted silhouette by up to theta = 180/V degrees. A feature at radius R on
// an object of projected size S pixels moves by (S/2)*sin(theta) pixels. Sub-
// pixel requires sin(180/V) <= 2/S.
//
//   V =  8  ->  theta 22.5 deg  ->  sub-pixel to S =  5.2 px
//   V = 16  ->  theta 11.25 deg ->  sub-pixel to S = 10.2 px
//   V = 24  ->  theta  7.5 deg  ->  sub-pixel to S = 15.3 px
//
// The mesh ladder hands over at ~4 px (the terminal rung's own epsilon is
// radius/2 and a rung is selected while its error projects under pixel_budget,
// so the handover is a couple of pixels by construction). V = 8 has no headroom
// at all; V = 16 has 2.5x, which is what the lod_bias dial needs. V = 24 — the
// figure the reference impostor system used — buys headroom to 15 px for 50 %
// more memory that no ground-level pose reaches. Hence 16.
//
// CELLS. A 32x32 cell is Nyquist-adequate to a projected size of 32 px, three
// times past the 10 px the view count allows. Cells are therefore NOT the
// binding constraint and spending more on them cannot improve the impostor;
// the view count binds first. 4x4 supersampling inside each texel gives true
// fractional coverage in the alpha channel, so the silhouette is antialiased
// rather than stair-stepped at the size where it is actually seen.
//
// ELEVATION ROWS (2026-08-05). v1 baked ONE azimuth ring at the equator and
// billboarded cylindrically, on the argument that ground props are looked at
// from around rather than from above. Flying over StreamMountain is exactly the
// case that argument excludes, and it fails two ways at once: a vertical card
// seen from 60 degrees up is foreshortened to half its height while the mesh it
// replaced is not, and the normals it hands the shader are the equator's, which
// point sideways when the light and the eye are both above. That is the
// impostor-vs-prefab brightness divergence, and it is structural — no amount of
// tuning the shade fixes a card showing the wrong face.
//
// So: 3 elevation rings at 0 / 30 / 60 degrees, nearest-selected, and the card
// billboards SPHERICALLY (it faces the eye instead of staying vertical). The
// two go together — rings without a tilting card still draw the right image on
// an edge-on quad, and a tilting card without rings draws the equator's image
// where the top should be.
//
// Above 75 degrees the nearest ring is 60 and the error grows to 30 degrees at
// straight-down. That IS the documented v1 limit now, moved from "any elevation
// at all" to "steeper than looking down at 75 degrees".
//
// COST: NOTHING. The cell was never the binding constraint — 32x32 is Nyquist-
// adequate to 32 px, three times past the ~10 px the azimuth count allows — so
// the resolution the cells did not need pays for the rings:
//
//                v1                        now
//   cells        4x4 of 32x32              8x8 of 16x16
//   views        16 az x 1 elev = 16       16 az x 3 elev = 48
//   layer        128 x 128                 128 x 128        (unchanged)
//   bytes        131,072 = 128 KiB         131,072 = 128 KiB (unchanged)
//
// Azimuth count is deliberately UNCHANGED at 16: it is the binding constraint
// (see VIEWS above), and spending it on elevation would trade a defect that
// shows at every camera angle for one that shows at some. The cells are now
// Nyquist-adequate to 16 px against the ~10 px the views allow — still not
// binding, but no longer three times clear of it, so the next feature to want
// atlas space should take one of the 16 unused cells rather than shrink these.
//
// ---------------------------------------------------------------------------
// WHAT IS IN THE ATLAS
//
// NOT albedo. The impostor keeps the engine's material path: it stores the
// surface's shading inputs and lets gbuffer.frag resolve colour through the
// same MaterialGpu record every other surface uses, so an impostor rock is lit
// by the rock's material and a live material edit reaches it.
//
//   layer 0 "shade" : R,G = octahedral surface normal (object space)
//                     B   = baked AO
//                     A   = coverage (fractional, 4x4 supersampled)
//   layer 1 "tint"  : R,G,B = TriEx tint rgb, A = tint blend strength
//
// The material INDEX is per-cluster (area-weighted mode over covered texels),
// not per-texel: a 10 px billboard cannot resolve a material boundary, and a
// per-texel material channel is the obvious v2 extension if a multi-material
// part ever needs one.
//
// ---------------------------------------------------------------------------
// DETERMINISM
//
// The bake is a fixed-order scanline rasterization in float arithmetic over a
// fixed triangle order with a strict depth test and a deterministic tie-break;
// no threads, no hash-map iteration, no time or RNG. Two cold bakes produce
// byte-identical atlases (verified with cmp, the .gtex double-bake discipline).

#include "tri.h"

#include <cstddef>
#include <cstdint>

#include "version_vector.h"   // M4: the single version-vector fold
#include <string>
#include <vector>

namespace impostor {

// Bump when anything that changes the atlas BYTES changes: the view layout,
// the cell size, the channel packing, or the rasterizer. Folded into the
// sidecar's identity word, so a stale atlas is detected and rebaked rather
// than silently drawn.
// M4: an alias of the version vector's impostor-format component. The atlas
// header still records it (a truncated/mismatched file must fail to parse),
// but the CACHE identity now comes from the vector through the one fold site.
constexpr uint32_t kFormatVersion = matter_version::components::kImpostorFormat;

constexpr uint32_t kAzimuths   = 16;                  // views per elevation ring
constexpr uint32_t kElevations = 3;                   // rings, see above
constexpr uint32_t kViews    = kAzimuths * kElevations;   // 48
constexpr uint32_t kCellPx   = 16;                    // per-view cell edge
constexpr uint32_t kGridDim  = 8;                     // kGridDim^2 >= kViews
constexpr uint32_t kLayerPx  = kCellPx * kGridDim;    // 128
constexpr uint32_t kSuperSample = 4;                  // per axis, per texel
constexpr size_t   kLayerBytes = size_t(kLayerPx) * kLayerPx * 4;
constexpr size_t   kAtlasBytes = kLayerBytes * 2;     // shade + tint

// Ring spacing, radians. Ring r is baked at r * kElevationStep above the
// equator; the runtime picks the NEAREST ring, so the boundaries sit halfway
// between (15 and 45 degrees for the 0/30/60 rings). The bake and the vertex
// stage must agree on this exactly, so it is written once here and mirrored
// into shaders_vk/impostor_common.glsl under a static_assert.
constexpr float kElevationStep = 0.52359877559829887308f;   // 30 degrees

// The view index for one (azimuth, elevation) pair. Azimuth-major so a ring is
// contiguous, which is what makes the runtime's index arithmetic a multiply-add
// rather than a lookup.
constexpr uint32_t view_index(uint32_t azimuth, uint32_t elevation) {
    return elevation * kAzimuths + azimuth;
}

static_assert(kGridDim * kGridDim >= kViews, "view grid too small");
static_assert(kLayerPx == 128, "atlas layer size is a format constant");
static_assert(kAtlasBytes == 131072, "elevation rings must stay byte-neutral");

// Eligibility floor: the terminal mesh rung must carry at least this many
// triangles for a 2-triangle billboard to be worth its atlas. See the header
// note for the derivation and the RockGallery measurement.
constexpr size_t kMinTerminalTris = 16;

// One cluster's impostor: the billboard's placement and its atlas.
struct ClusterImpostor {
    uint32_t cluster_index = 0;      // index into the flat artifact's cluster table
    float    center[3]     = {0, 0, 0};   // part-local billboard centre
    float    half_extent   = 0.0f;        // part-local half-size of the square quad
    uint32_t material_index = 0;          // dominant material over covered texels
    uint32_t source_tris   = 0;           // terminal mesh rung's triangle count
    std::vector<uint8_t> atlas;           // kAtlasBytes: layer 0 then layer 1
};

struct PartImpostor {
    std::vector<ClusterImpostor> clusters;
    bool empty() const { return clusters.empty(); }
};

// True when this cluster's terminal mesh rung earns an impostor.
inline bool cluster_earns_impostor(size_t terminal_tris) {
    return terminal_tris >= kMinTerminalTris;
}

// Rasterize `tris`/`triex` into one ClusterImpostor. `cluster_index` and the
// triangle count are recorded on the result. Returns false (and leaves `out`
// untouched) for an empty mesh or a degenerate extent.
bool bake_cluster(uint32_t cluster_index, const std::vector<Tri>& tris,
                  const std::vector<TriEx>& triex, ClusterImpostor& out);

// The content the atlas DEPICTS, folded to one word: every terminal-rung
// triangle's vertex bytes, in cluster order, plus the bake parameters that
// decide what the rasterizer produces. This is the cache identity — an atlas
// whose depicts-hash does not match the ladder it is loaded beside is stale by
// definition, and is rejected (and logged) rather than drawn.
uint64_t depicts_hash_begin();
void     depicts_hash_add_cluster(uint64_t& h, uint32_t cluster_index,
                                  const std::vector<Tri>& tris);
uint64_t depicts_hash_finish(uint64_t h);

// "parts/<16-hex>.bundle" — the IMPO section of the part's bundle, same
// content-addressed discipline as cache_path_lods / cache_path_hints.
std::string cache_path_impostor(uint64_t resolved_hash);

bool save(const std::string& path, uint64_t part_hash, uint64_t depicts_hash,
          const PartImpostor& in);

// Why a load produced nothing. Every value other than None is a condition the
// caller MUST log once, naming the part — an entire rendering tier went
// silently missing for a generation of artifacts on the abandoned branch
// because this enum did not exist and the failure was swallowed by a bare
// `return` inside a `catch (...)`.
enum class LoadFailure : uint8_t {
    None = 0,
    Absent,       // no sidecar (a part with no eligible cluster: not an error)
    Open,         // present but unreadable
    Header,       // bad magic
    Version,      // written by a different impostor format
    Identity,     // part hash mismatch (sidecar belongs to another part)
    Stale,        // depicts-hash mismatch: the mesh it depicts has changed
    Truncated,    // short read / declared size past end of file
    Checksum,     // payload checksum mismatch (bit rot, partial write)
    Malformed,    // structurally impossible field (zero clusters, bad extent)
};

const char* load_failure_text(LoadFailure f);

// Reads the sidecar and validates it against (part_hash, depicts_hash).
// Returns false with *fail set on every rejection; `reason` receives a short
// human string naming what was wrong. `out` is cleared on failure.
bool load(const std::string& path, uint64_t part_hash, uint64_t depicts_hash,
          PartImpostor& out, LoadFailure* fail = nullptr,
          std::string* reason = nullptr);

// The two triangles of the billboard, in part-local space, as the ladder rung's
// geometry. The quad is authored flat in the XY plane about `center` with the
// half-extent baked in; the vertex stage re-orients it to face the camera, so
// these positions exist to give the rung a real AABB, a real BLAS and a real
// index/vertex range — everything downstream treats it as a mesh.
//
// TriEx carries the impostor marker: uv0.x holds kQuadMarker and uv0.y the
// corner ordinal, which build_raster_mesh_data forwards into the vertex
// stream. materialId is the cluster's dominant material.
constexpr float kQuadMarker = 1.0e30f;

void build_quad(const ClusterImpostor& imp, std::vector<Tri>& tris,
                std::vector<TriEx>& triex);

// Is this ladder rung the billboard rather than a mesh? Two triangles carrying
// the marker. Every consumer that walks a ladder as GEOMETRY has to ask -- a
// billboard is only a picture when the vertex stage orients it, so merging one
// into another part's mesh (part_flatten's segmented child inlining) or
// charting one for VT would both be nonsense. One predicate, so the answer
// cannot drift between the two callers.
inline bool is_billboard_rung(const std::vector<Tri>& tris,
                              const std::vector<TriEx>& triex) {
    return tris.size() == 2 && triex.size() == 2 &&
           triex[0].uv0.x >= kQuadMarker;
}

} // namespace impostor
