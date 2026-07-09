#pragma once
// ParticleFlowLib: generic agent-particle kernel + path recording.
// No engine dependencies. All state is instance-contained (no globals/statics):
// N sims run concurrently on bake worker threads with zero coordination.
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>
#include <memory>

namespace pf {

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
    std::vector<std::string> channel_names;         // fixed at construction
    std::vector<Path> paths;
};

// ---------------------------------------------------------------------------
// Sim configuration
// ---------------------------------------------------------------------------
enum class FieldType { Bias, Curl, Adhere, Attract, Separate, Drag };
enum class FieldMode { Steer, Force };

// Optional axial weight fade: multiplier 1 where dot(p,axis) <= from,
// 0 where >= to, linear between. Disabled by default.
struct Fade { V3 axis{0,1,0}; float from = 0, to = 0; bool enabled = false; };

struct FieldConfig {
    FieldType type = FieldType::Bias;
    FieldMode mode = FieldMode::Steer;
    float weight = 1.0f;
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

struct EmitterConfig {
    int shape = 1;                // 0=point 1=disc 2=ring
    V3 center{0,0,0}, axis{0,1,0};
    float radius = 0.5f;
    float rate = 1.0f;            // particles per tick (fractions accumulate)
    float vel0 = 1.0f;            // initial speed along axis
    float jitter = 0.0f;          // random velocity magnitude added
    std::vector<float> attr_init; // one per declared channel (missing -> 0)
};

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
    std::vector<EmitterConfig> emitters;
    std::vector<FieldConfig> fields;
};

class Sim;
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
    void attach(ITickObserver* o);                     // not owned
    void set_attractors(const float* xyz, size_t n);   // appends n points
    void run(uint32_t n_ticks);                        // callable repeatedly

    uint32_t slot_count() const { return (uint32_t)id_.size(); }
    uint32_t alive_count() const { return alive_n_; }
    uint32_t tick() const { return tick_; }
    float* pos_data() { return pos_.data(); }
    float* vel_data() { return vel_.data(); }
    uint8_t* alive_data() { return alive_.data(); }
    const float* pos_data() const { return pos_.data(); }
    const uint8_t* alive_data() const { return alive_.data(); }
    float* attr_data(uint32_t ch) { return attrs_[ch].data(); }
    const float* attr_data(uint32_t ch) const { return attrs_[ch].data(); }
    uint32_t channel_count() const { return (uint32_t)attrs_.size(); }
    int channel_index(const std::string& name) const;
    uint32_t id_of(uint32_t slot) const { return id_[slot]; }
    const std::vector<uint32_t>& born_this_tick() const { return born_; }
    const std::vector<uint32_t>& died_this_tick() const { return died_; }

    uint32_t emit_particle(V3 pos, V3 vel, const float* attr_or_null);
    void kill(uint32_t slot);
    void set_field_weight(uint32_t field_index, float w);
    uint32_t field_count() const { return (uint32_t)cfg_.fields.size(); }
    uint32_t attractors_remaining() const { return attr_remaining_; }

    size_t deposited_count() const { return deposited_pts_.size(); }
    const std::vector<V3>& deposited_points() const { return deposited_pts_; }
    const SpatialHash& deposited_hash() const { return dep_hash_; }
    const SpatialHash& live_hash() const { return live_hash_; }
    // Outward surface normal estimate from the deposited neighborhood.
    // *ok=false when no deposited points lie within radius. (Impl: Task 3.)
    V3 surface_normal(V3 p, float radius, bool* ok) const;

    const SimConfig& config() const { return cfg_; }
    Rng& rng() { return rng_; }

private:
    void step();
    void run_emitters();
    void integrate_slot(uint32_t i);
    void kill_slot(uint32_t i);
    void deposit(V3 p);
    float fade_mult(const FieldConfig& f, V3 p) const;
    V3 attract_dir(uint32_t slot, V3 p);   // consumes attractors (Task 3)

    SimConfig cfg_;
    Rng rng_;
    uint32_t tick_ = 0, next_id_ = 0, alive_n_ = 0;
    std::vector<float> pos_, vel_;
    std::vector<std::vector<float>> attrs_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> id_, age_;
    std::vector<float> dep_dist_;
    std::vector<uint32_t> free_slots_;
    std::vector<uint32_t> born_, died_;
    std::vector<float> emit_acc_;
    std::vector<V3> deposited_pts_;         // append-only
    SpatialHash dep_hash_, live_hash_;
    std::vector<V3> attractors_;            // append-only
    std::vector<uint8_t> attr_consumed_;
    uint32_t attr_remaining_ = 0;
    std::vector<ITickObserver*> observers_;
};

} // namespace pf
