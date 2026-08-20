#pragma once
// ParticleFlowLib: generic agent-particle kernel + path recording.
// No engine dependencies. All state is instance-contained (no globals/statics):
// N sims run concurrently on bake worker threads with zero coordination.
//
// libs/ParticleFlowLib/include/particle_flow.h
//
// The model
// ---------
// Particles are velocity-canonical: each carries a position and a velocity,
// and fields never write position directly. A field contributes either
//   - a STEER direction (FieldMode::Steer) — unit directions from every steer
//     field are summed with their weights, the sum is normalized into a
//     `desired` heading, and the velocity is ROTATED toward it by at most
//     `SimConfig::max_turn_rate` radians per tick. Speed is preserved.
//   - a FORCE (FieldMode::Force) — added to the velocity before steering, so
//     it does change speed.
// Every particle also drops "deposit" points along its path (one every
// `deposit_every` units of travelled distance) into an append-only cloud plus
// a spatial hash. The neighborhood fields (Adhere, Align) read that cloud
// back, so later strands accrete onto the structure earlier strands laid
// down. That feedback loop is the point of the library.
//
// Determinism
// -----------
// Same config + same seed => bit-identical output. It rests on three things:
// the instance-owned xoshiro256++ RNG (`Rng`, no global state), ascending slot
// iteration everywhere, and a fixed `dt`. `SpatialHash` (pf_spatial_hash.h)
// is deterministic for the same reason. Anything that reorders slot iteration
// or draws from `rng()` out of band breaks reproducibility of every cached
// bake that used this kernel.
//
// Lifecycle
// ---------
//   SimConfig cfg; ...                       // attributes/state/emitters/fields
//   pf::Sim sim(cfg);                        // allocates and reserves up front
//   pf::PathRecorder rec(min_seg, names);
//   sim.attach(&rec);                        // observer, NOT owned
//   sim.set_attractors(xyz, n);              // optional, appendable
//   sim.run(ticks);                          // repeatable; state persists
//   const pf::PathSet& out = rec.paths();
//
// Units and spaces
// ----------------
// The library imposes NO units or coordinate convention: positions, radii,
// `deposit_every` and `hash_cell` are all in whatever space the caller feeds
// it. `dt` and `max_age` are in ticks; `max_turn_rate` is radians per tick;
// `speed_relax` is a fraction per tick.
//
// Threading and lifetime
// ----------------------
// One `Sim` is single-threaded. Instances share nothing (no globals, no
// statics), which is what lets N sims run on N bake workers with no locking.
// `pos_data()`/`vel_data()`/`attr_data()`/`state_data()` hand out RAW pointers
// into internal vectors; those vectors are reserved to `max_particles` in the
// constructor precisely so they never reallocate, so the pointers stay valid
// for the Sim's lifetime. `emit_particle` refuses (returns UINT32_MAX) rather
// than growing past `max_particles`, which is what upholds that invariant.
//
// Memory grows monotonically with a run, not with the live population: the
// deposit cloud and its hash are append-only, attractors are append-only, and
// `PathRecorder` indexes by particle id (which only ever increases).
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>
#include <memory>

namespace pf {

// Plain 12-byte vector — deliberately NOT MathLib's `mm::Vec3` nor
// SpatialQueryLib's SIMD `float3`. ParticleFlowLib is a leaf with no repo
// dependencies, so it carries its own minimal type; callers converting at the
// boundary (see `MatterEngine3/src/pf_bindings.cpp`) do so field by field.
struct V3 { float x = 0, y = 0, z = 0; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(V3 a);
V3 normalize(V3 a);   // zero-safe: returns {0,0,0} for near-zero input

} // namespace pf

#include "pf_spatial_hash.h"

namespace pf {

// xoshiro256++ seeded via splitmix64. Deterministic, instance-owned.
struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed);
    uint64_t next_u64();
    float next_unit();                  // [0, 1)
    float range(float a, float b);
    V3 unit_sphere();                   // uniform direction on the unit sphere
};

// Append-only polylines with per-vertex attribute channels.
// Monotonic accretion is enforced structurally: vertices are only appended,
// paths are only appended, existing data is never mutated.
struct PathSet {
    struct Path {
        uint32_t particle_id = 0;
        std::vector<float> xyz;                     // 3 floats per vertex
        std::vector<std::vector<float>> channels;   // [channel][vertex]
        bool closed = false;                        // particle died / finalized
        size_t vertex_count() const { return xyz.size() / 3; }
    };
    // Labels for the per-vertex channels, in `Path::channels` order. Supplied
    // by whoever constructs the `PathRecorder`; the recorder sizes each path's
    // `channels` from the Sim's attribute COUNT and never cross-checks the two,
    // so a name list of a different length silently mislabels the data.
    std::vector<std::string> channel_names;         // fixed at construction
    std::vector<Path> paths;
};

// ---------------------------------------------------------------------------
// Sim configuration
// ---------------------------------------------------------------------------
// What a field computes. Each type reads only a subset of `FieldConfig`; the
// rest of that struct is ignored.
//   Bias      constant direction `dir`.
//   Curl      curl of a seeded value-noise vector potential, sampled at the
//             particle position divided by `scale` (bigger scale = bigger
//             swirls). Divergence-free, so it stirs without pooling.
//   Adhere    steer toward the centroid of deposited points within `radius`,
//             offset back out along the estimated surface normal by
//             `surface_offset` — i.e. ride just outside existing structure.
//   Align     steer along the average recorded HEADING of deposited points
//             within `radius`. Adhere alone lets a follower orbit a trunk;
//             Align makes it travel the way its predecessors travelled.
//   Attract   steer at the nearest unconsumed attractor within `influence`;
//             the attractor is consumed within `kill_radius` and the particle
//             optionally dies with it (`kill_on_consume`).
//   Separate  inverse-square push away from LIVE neighbors within `radius`.
//   Drag      velocity * -`k`. Force-only.
//
// Not every type works in every mode (see FieldMode). Adhere/Align/Separate
// produce nothing in Force mode, Drag produces nothing in Steer mode, and
// Attract is only honored in Steer mode — a mismatched pairing silently
// disables the field rather than erroring.
enum class FieldType { Bias, Curl, Adhere, Align, Attract, Separate, Drag };
// How the field's output enters integration.
//   Steer  a unit direction added (weighted) to the blended desired heading;
//          the velocity is then rotated toward that heading by at most
//          `SimConfig::max_turn_rate` per tick. Speed is unchanged.
//   Force  a vector added to the velocity (scaled by `dt`), before steering.
//          This is the only way a field changes speed.
enum class FieldMode { Steer, Force };

// Optional axial weight fade: multiplier 1 where dot(p,axis) <= from,
// 0 where >= to, linear between. Disabled by default.
struct Fade { V3 axis{0,1,0}; float from = 0, to = 0; bool enabled = false; };

// One field's parameters. This is a union-by-convention: most members are read
// by exactly one `FieldType` (noted per line) and ignored by the rest, so an
// unused member's value never matters.
//
// Effective per-particle weight, applied every tick in `Sim::integrate_slot`:
//     weight * fade_mult(fade, position) * max(state[weight_state], 0)
// A zero product short-circuits the field entirely — it is not evaluated at
// all — which makes `weight_state` both a scripting hook and the cheapest way
// to disable an expensive neighborhood field per particle.
//
// `weight` is not frozen at construction: `Sim::set_field_weight` mutates it
// live, so a config handed to a Sim is a mutable starting point, not a record
// of what the Sim is currently doing.
struct FieldConfig {
    FieldType type = FieldType::Bias;
    FieldMode mode = FieldMode::Steer;
    float weight = 1.0f;
    // Optional per-particle weight multiplier: index into SimConfig::state
    // channels (-1 = off). Effective weight = weight * fade * max(state, 0),
    // so scripts can gate or blend any field per particle from onTick.
    int weight_state = -1;
    Fade fade;
    V3 dir{0,1,0};                // Bias direction
    float radius = 1.0f;          // Adhere/Separate neighborhood radius
    float surface_offset = 0.0f;  // Adhere: ride this far outside the surface
    float influence = 1.0f;       // Attract: capture radius for steering
    float kill_radius = 0.1f;     // Attract: consume distance
    bool kill_on_consume = true;  // Attract: strand terminates at its attractor
    float scale = 1.0f;           // Curl: spatial noise scale
    uint32_t seed = 0;            // Curl: noise seed
    float k = 0.0f;               // Drag coefficient
};

// One emitter. Runs at the start of every tick, before integration, so a
// particle born this tick also moves this tick.
//
// `rate` is fractional and accumulates: 0.25 emits one particle every fourth
// tick, never a quarter of one. The disc shape samples uniformly by AREA
// (radius scaled by sqrt of a uniform draw), the ring samples the rim exactly;
// both use a basis built from `axis`, which is normalized internally.
//
// `attr_init`/`state_init` may name every declared channel — there is no
// channel-count ceiling. A vector shorter than the sim's channel list leaves
// the remaining channels at 0, and entries past the sim's channel count are
// ignored.
struct EmitterConfig {
    int shape = 1;                // 0=point 1=disc 2=ring
    V3 center{0,0,0}, axis{0,1,0};
    float radius = 0.5f;
    float rate = 1.0f;            // particles per tick (fractions accumulate)
    float vel0 = 1.0f;            // initial speed along axis
    float jitter = 0.0f;          // random velocity magnitude added
    std::vector<float> attr_init; // one per declared channel (missing -> 0)
    std::vector<float> state_init;// one per declared state channel (missing -> 0)
};

// The complete description of a sim. Copied into the `Sim` at construction;
// the copy is what the Sim runs against, and only `fields[i].weight` is
// mutable afterwards (via `Sim::set_field_weight`). Everything else — channel
// lists, emitters, caps, `hash_cell` — is fixed once the Sim exists.
//
// Sizing decisions taken once, at construction, from these values:
//   - `max_particles` sets the reservation of every per-particle buffer and is
//     a HARD cap: `emit_particle` returns UINT32_MAX beyond it rather than
//     growing, which is what keeps the raw data pointers stable.
//   - `hash_cell` (or, at 0, the largest Adhere/Align/Separate `radius` and
//     Attract `influence` across `fields`, defaulting to 1) sets both spatial
//     hashes' cell size for the whole run. Later weight changes do not resize
//     them.
//   - `attributes` are per-particle floats RECORDED into paths; `state` are
//     per-particle floats that are not. Both are addressed by index, so their
//     order is part of the contract with the script that reads them.
struct SimConfig {
    uint64_t seed = 1;
    float dt = 1.0f;
    float max_turn_rate = 0.15f;  // radians/tick steer clamp
    float speed_target = -1.0f;   // <0 = no speed regulation
    float speed_relax = 0.1f;     // fraction/tick toward speed_target
    float deposit_every = 0.1f;   // distance between deposited points
    uint32_t max_age = 0;         // ticks; 0 = unlimited
    uint32_t max_particles = 16384;
    float hash_cell = 0.0f;       // 0 = auto (max field neighborhood radius)
    std::vector<std::string> attributes;
    // Script-state channels: per-particle floats like attributes, but NOT
    // recorded into paths. Free space for onTick policies (branch flags,
    // cooldowns, per-particle field weights via FieldConfig::weight_state...).
    std::vector<std::string> state;
    std::vector<EmitterConfig> emitters;
    std::vector<FieldConfig> fields;
};

class Sim;
// Called once at the END of every `Sim::step`, in `attach` order, with the
// post-integration state of that tick. Observers are NOT owned by the Sim and
// must outlive it. The callback sees the Sim as const but may read
// `born_this_tick()` / `died_this_tick()`, which are valid only during this
// call — both are cleared at the top of the next step. `PathRecorder` below is
// the reference implementation.
struct ITickObserver {
    virtual ~ITickObserver() = default;
    virtual void on_tick(const Sim& s, uint32_t tick) = 0;
};

// ---------------------------------------------------------------------------
// Sim: velocity-canonical agent-particle kernel. Deterministic: ascending
// slot iteration, instance-owned RNG, fixed dt. Single-threaded per instance;
// instances are fully independent (safe on concurrent bake workers).
// ---------------------------------------------------------------------------
class Sim {
public:
    explicit Sim(SimConfig cfg);
    // `set_attractors` copies n xyz triples (3 floats each) and marks them
    // unconsumed; it may be called repeatedly and mid-run, and never removes.
    // Consumed attractors stay in the array as tombstones, and both the
    // per-tick Attract steering and `nearest_attractor` scan it linearly, so
    // cost is O(total ever added) per particle per tick, not O(remaining).
    //
    // `run` advances n ticks and returns; Sim state (tick counter, particles,
    // deposits, RNG) persists, so run(10) twice equals run(20). Observers fire
    // once per tick from inside.
    void attach(ITickObserver* o);                     // not owned
    void set_attractors(const float* xyz, size_t n);   // appends n points
    void run(uint32_t n_ticks);                        // callable repeatedly

    // Slots vs ids. A SLOT is a position in the parallel per-particle arrays
    // and is RECYCLED: when a particle dies its slot returns to a free list and
    // a later emit reuses it. `slot_count()` is therefore the high-water mark
    // of simultaneously-resident particles, never decreases, and is bounded by
    // `max_particles`. A slot index only means anything while
    // `alive_data()[slot]` is 1. An ID is assigned once per particle, is dense
    // and monotonically increasing for the life of the Sim, and is what
    // `PathRecorder` keys on to keep one strand per particle.
    uint32_t slot_count() const { return (uint32_t)id_.size(); }
    uint32_t alive_count() const { return alive_n_; }
    uint32_t tick() const { return tick_; }
    float* pos_data() { return pos_.data(); }
    float* vel_data() { return vel_.data(); }
    uint8_t* alive_data() { return alive_.data(); }
    const float* pos_data() const { return pos_.data(); }
    const float* vel_data() const { return vel_.data(); }
    const uint8_t* alive_data() const { return alive_.data(); }
    float* attr_data(uint32_t ch) { return attrs_[ch].data(); }
    const float* attr_data(uint32_t ch) const { return attrs_[ch].data(); }
    uint32_t channel_count() const { return (uint32_t)attrs_.size(); }
    int channel_index(const std::string& name) const;
    float* state_data(uint32_t ch) { return states_[ch].data(); }
    const float* state_data(uint32_t ch) const { return states_[ch].data(); }
    uint32_t state_count() const { return (uint32_t)states_.size(); }
    int state_index(const std::string& name) const;
    uint32_t id_of(uint32_t slot) const { return id_[slot]; }
    const std::vector<uint32_t>& born_this_tick() const { return born_; }
    const std::vector<uint32_t>& died_this_tick() const { return died_; }

    // Spawn one particle immediately (outside the emitter schedule) and return
    // its slot, or UINT32_MAX if the sim is already at `max_particles` — a
    // normal, expected outcome that callers must check. `attr_or_null` /
    // `state_or_null`, when non-null, must point at `channel_count()` /
    // `state_count()` floats; null fills zeros. Side effect: the spawn position
    // is deposited into the point cloud right away, so a particle contributes
    // to Adhere/Align neighborhoods from tick zero.
    uint32_t emit_particle(V3 pos, V3 vel, const float* attr_or_null,
                           const float* state_or_null = nullptr);
    // Kill an already-alive slot (no-op otherwise). Releases any attractor
    // claim, frees the slot for reuse and appends it to `died_this_tick()`.
    // The slot's position and velocity are left intact so the observer can read
    // the death position at the end of the tick.
    void kill(uint32_t slot);
    void set_field_weight(uint32_t field_index, float w);
    uint32_t field_count() const { return (uint32_t)cfg_.fields.size(); }
    uint32_t attractors_remaining() const { return attr_remaining_; }

    // Script-driven attractor reservation. A claimed attractor is skipped by
    // the generic nearest-attractor steering; its claimer beelines to it.
    // Claims are released if the claimer dies before consuming.
    int nearest_attractor(V3 p, float max_dist, bool unclaimed_only) const;
    bool claim_attractor(uint32_t slot, uint32_t attractor_idx);
    V3 attractor_pos(uint32_t idx) const { return attractors_[idx]; }
    uint32_t attractor_count() const { return (uint32_t)attractors_.size(); }
    // Count of deposited points within radius (open-space probes for scripts).
    uint32_t deposit_near_count(V3 p, float radius) const;

    size_t deposited_count() const { return deposited_pts_.size(); }
    const std::vector<V3>& deposited_points() const { return deposited_pts_; }
    // Unit heading of the depositing particle at deposit time (parallel to
    // deposited_points). Align fields steer along these.
    const std::vector<V3>& deposited_dirs() const { return deposited_dirs_; }
    const SpatialHash& deposited_hash() const { return dep_hash_; }
    const SpatialHash& live_hash() const { return live_hash_; }
    // Outward surface normal estimate from the deposited neighborhood.
    // *ok=false when no deposited points lie within radius. Note the estimate
    // also degenerates to zero when p sits at the neighborhood's centroid, and
    // that case still reports ok = true.
    V3 surface_normal(V3 p, float radius, bool* ok) const;

    // The live config, including any `set_field_weight` mutations.
    const SimConfig& config() const { return cfg_; }
    // The sim's own RNG, exposed so scripts can make decisions that stay part
    // of the reproducible stream. Drawing from it shifts every subsequent
    // emitter draw, so a script that samples it conditionally makes the run
    // reproducible only for the same conditions.
    Rng& rng() { return rng_; }

private:
    void step();
    void run_emitters();
    void integrate_slot(uint32_t i);
    void kill_slot(uint32_t i);
    void deposit(V3 p, V3 dir);
    float fade_mult(const FieldConfig& f, V3 p) const;
    // Attract steering for ONE configured Attract field. `f` is the field
    // currently being evaluated, so several Attract fields with different
    // influence / kill_radius / kill_on_consume each behave as configured.
    // Mutates the sim: reaching an attractor consumes it and may kill `slot`.
    V3 attract_dir(const FieldConfig& f, uint32_t slot, V3 p);

    SimConfig cfg_;
    Rng rng_;
    uint32_t tick_ = 0, next_id_ = 0, alive_n_ = 0;
    std::vector<float> pos_, vel_;
    std::vector<std::vector<float>> attrs_;
    std::vector<std::vector<float>> states_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> id_, age_;
    std::vector<uint32_t> claim_of_;        // per-slot claimed attractor (UINT32_MAX = none)
    std::vector<float> dep_dist_;
    std::vector<uint32_t> free_slots_;
    std::vector<uint32_t> born_, died_;
    std::vector<float> emit_acc_;
    std::vector<V3> deposited_pts_;         // append-only
    std::vector<V3> deposited_dirs_;        // parallel to deposited_pts_
    SpatialHash dep_hash_, live_hash_;
    std::vector<V3> attractors_;            // append-only
    std::vector<uint8_t> attr_consumed_;
    std::vector<uint8_t> attr_claimed_;
    uint32_t attr_remaining_ = 0;
    std::vector<ITickObserver*> observers_;
};

// Records each particle's trajectory into an append-only PathSet.
// Vertices are appended when a particle has moved >= min_segment from its
// last recorded vertex (plus the spawn vertex and the final position at death).
// Attach with `Sim::attach` before running; the Sim does not own it, so it must
// outlive the Sim (or at least the run). Memory grows with the number of
// particles EVER emitted, not the live count: the id->track table is indexed by
// the monotonically increasing particle id and is never compacted.
//
// A `min_segment` of 0 (or negative, which is clamped to 0) disables the
// distance test entirely, leaving only the spawn vertex and the death vertex
// per path — not "record every tick".
class PathRecorder : public ITickObserver {
public:
    PathRecorder(float min_segment, const std::vector<std::string>& channel_names);
    void on_tick(const Sim& s, uint32_t tick) override;
    const PathSet& paths() const { return set_; }

private:
    struct Track { uint32_t path_index; V3 last; };
    float min_seg_;
    PathSet set_;
    std::vector<Track> by_id_;      // particle id -> track (ids are dense)
    std::vector<uint8_t> known_;    // particle id known?
    void append_vertex(const Sim& s, uint32_t slot, uint32_t path_index);
};

// Unit direction of the last segment ({0,0,0} for single-vertex paths).
V3 path_end_dir(const PathSet::Path& p);

} // namespace pf
