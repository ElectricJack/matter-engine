// libs/ParticleFlowLib/src/pf_sim.cpp
//
// The `Sim` kernel: everything except field evaluation (`pf_fields.cpp`) and
// path recording (`pf_path_recorder.cpp`).
//
// Tick order — `Sim::step`, and the order matters:
//   1. clear `born_`/`died_`, increment the tick counter
//   2. run emitters (so a particle born this tick also integrates this tick)
//   3. rebuild `live_hash_` from START-of-tick positions
//   4. integrate every alive slot in ascending order
//   5. notify observers
// Step 3 is the reason Separate is order-independent: a particle integrated
// later in step 4 still sees its already-moved neighbors at their old
// positions. Deliberate, and the cheaper option too.
//
// Slot lifecycle. Slots are handed out from `free_slots_` when available and
// appended otherwise, so `slot_count()` is a high-water mark and stays at or
// below `max_particles` (every slot is either alive or free, and a new one is
// only appended when the free list is empty and the sim is under its cap).
// That invariant is load-bearing: the constructor reserves every per-particle
// buffer to `max_particles` so the raw pointers `pos_data()` and friends hand
// out never dangle, and only that invariant keeps the vectors from growing.
//
// Nothing here locks. One Sim is single-threaded; separate Sims share no state.
#include "particle_flow.h"
#include <algorithm>
#include <cmath>

namespace pf {

// Implemented in pf_fields.cpp: Bias, Curl, Adhere, Align and Separate as
// steer directions, Bias, Curl and Drag as forces. A type/mode pair neither
// switch covers returns {0,0,0}, which is a normal "nothing to say" result and
// not an error. Attract is handled by Sim::attract_dir instead, because it
// mutates sim state.
V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot);
V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot);

static inline V3 read3(const std::vector<float>& a, uint32_t i) {
    return {a[3*i], a[3*i+1], a[3*i+2]};
}
static inline void write3(std::vector<float>& a, uint32_t i, V3 v) {
    a[3*i] = v.x; a[3*i+1] = v.y; a[3*i+2] = v.z;
}

// Spatial-hash cell size when `SimConfig::hash_cell` is left at 0: the largest
// neighborhood radius any field will actually query with (Adhere/Separate/Align
// `radius`, Attract `influence`), falling back to 1 when no field queries at
// all. Sizing the cell to the largest query keeps every query to a 2x2x2-ish
// cell walk.
//
// Computed ONCE, in the constructor, and used for both hashes. Nothing resizes
// them afterwards, so raising a radius at runtime makes queries walk more cells
// — still correct, just slower.
static float auto_cell(const SimConfig& c) {
    float r = 0.0f;
    for (const auto& f : c.fields) {
        if (f.type == FieldType::Adhere || f.type == FieldType::Separate ||
            f.type == FieldType::Align)
            r = std::max(r, f.radius);
        if (f.type == FieldType::Attract)
            r = std::max(r, f.influence);
    }
    return r > 1e-6f ? r : 1.0f;
}

// Takes the config by value and keeps it — `cfg_` is the sim's live
// configuration from here on, and `set_field_weight` writes into it.
//
// Both spatial hashes get the same cell size. The reservations below are not an
// optimization: see the file header on why the per-particle buffers must never
// reallocate. Note `deposited_pts_`/`deposited_dirs_` are NOT reserved — they
// are unbounded by design and grow for the length of the run.
Sim::Sim(SimConfig cfg)
    : cfg_(std::move(cfg)), rng_(cfg_.seed),
      dep_hash_(cfg_.hash_cell > 0 ? cfg_.hash_cell : auto_cell(cfg_)),
      live_hash_(cfg_.hash_cell > 0 ? cfg_.hash_cell : auto_cell(cfg_)) {
    attrs_.resize(cfg_.attributes.size());
    states_.resize(cfg_.state.size());
    emit_acc_.assign(cfg_.emitters.size(), 0.0f);
    // pre-reserve to capacity: onTick views hold raw pointers; growth must never reallocate
    pos_.reserve(3 * cfg_.max_particles);
    vel_.reserve(3 * cfg_.max_particles);
    alive_.reserve(cfg_.max_particles);
    id_.reserve(cfg_.max_particles);
    age_.reserve(cfg_.max_particles);
    dep_dist_.reserve(cfg_.max_particles);
    claim_of_.reserve(cfg_.max_particles);
    for (auto& ch : attrs_) ch.reserve(cfg_.max_particles);
    for (auto& ch : states_) ch.reserve(cfg_.max_particles);
}

void Sim::attach(ITickObserver* o) { observers_.push_back(o); }

// Append n attractors from n xyz triples. Appendable mid-run; there is no
// removal, and consumed attractors remain in the array as tombstones. Every
// per-tick Attract query and every `nearest_attractor` call scans the whole
// array including tombstones, so this list is O(total added) forever.
void Sim::set_attractors(const float* xyz, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        attractors_.push_back({xyz[3*i], xyz[3*i+1], xyz[3*i+2]});
        attr_consumed_.push_back(0);
        attr_claimed_.push_back(0);
    }
    attr_remaining_ += (uint32_t)n;
}

int Sim::channel_index(const std::string& name) const {
    for (size_t i = 0; i < cfg_.attributes.size(); ++i)
        if (cfg_.attributes[i] == name) return (int)i;
    return -1;
}

int Sim::state_index(const std::string& name) const {
    for (size_t i = 0; i < cfg_.state.size(); ++i)
        if (cfg_.state[i] == name) return (int)i;
    return -1;
}

// Nearest unconsumed attractor within `max_dist`, or -1. Read-only — unlike
// `attract_dir` it never consumes, claims or kills; this is the query half of
// the script-driven claiming protocol. Linear over all attractors ever added.
int Sim::nearest_attractor(V3 p, float max_dist, bool unclaimed_only) const {
    int best = -1; float best_d2 = max_dist * max_dist;
    for (size_t i = 0; i < attractors_.size(); ++i) {
        if (attr_consumed_[i]) continue;
        if (unclaimed_only && attr_claimed_[i]) continue;
        V3 d = attractors_[i] - p;
        float d2 = dot(d, d);
        if (d2 < best_d2) { best_d2 = d2; best = (int)i; }
    }
    return best;
}

// Reserve an attractor for one particle: generic Attract steering then skips it
// for everyone else, and the claimer beelines to it ignoring `influence`.
// Returns false for a dead/out-of-range slot, or for an attractor that is
// already consumed or already claimed — all normal outcomes a script polls on.
// A slot may hold at most one claim; re-claiming releases the previous one, and
// `kill_slot` releases it if the claimer dies before consuming.
bool Sim::claim_attractor(uint32_t slot, uint32_t idx) {
    if (slot >= slot_count() || !alive_[slot]) return false;
    if (idx >= attractors_.size() || attr_consumed_[idx] || attr_claimed_[idx])
        return false;
    if (claim_of_[slot] != UINT32_MAX && !attr_consumed_[claim_of_[slot]])
        attr_claimed_[claim_of_[slot]] = 0;    // re-claim: release the old one
    attr_claimed_[idx] = 1;
    claim_of_[slot] = idx;
    return true;
}

uint32_t Sim::deposit_near_count(V3 p, float radius) const {
    uint32_t n = 0;
    dep_hash_.query(p, radius, [&](uint32_t, V3, float) { ++n; });
    return n;
}

// Lay down one point of "wood": into the query hash and both parallel arrays
// (position, and the normalized heading Align steers along). Append-only and
// never pruned, so the deposit cloud is the dominant memory term of a long run:
// roughly one entry per particle per `deposit_every` units travelled.
void Sim::deposit(V3 p, V3 dir) {
    dep_hash_.insert(p, (uint32_t)deposited_pts_.size());
    deposited_pts_.push_back(p);
    deposited_dirs_.push_back(normalize(dir));
}

// Allocate a slot (recycling a freed one when possible), initialize it and
// return the slot index; UINT32_MAX when already at `max_particles`. The cap
// test is on the ALIVE count, so a sim churning particles keeps emitting
// forever without growing its buffers.
//
// Appending a new slot must push to every parallel array in lockstep — they are
// all indexed by slot and any omission desynchronizes them silently.
//
// Side effect: deposits at the spawn position, so a new particle is part of the
// Adhere/Align neighborhood immediately rather than after its first move.
uint32_t Sim::emit_particle(V3 p, V3 v, const float* attr_or_null,
                            const float* state_or_null) {
    if (alive_n_ >= cfg_.max_particles) return UINT32_MAX;
    uint32_t slot;
    if (!free_slots_.empty()) { slot = free_slots_.back(); free_slots_.pop_back(); }
    else {
        slot = slot_count();
        pos_.resize(pos_.size() + 3); vel_.resize(vel_.size() + 3);
        for (auto& ch : attrs_) ch.push_back(0.0f);
        for (auto& ch : states_) ch.push_back(0.0f);
        alive_.push_back(0); id_.push_back(0); age_.push_back(0);
        dep_dist_.push_back(0.0f);
        claim_of_.push_back(UINT32_MAX);
    }
    write3(pos_, slot, p); write3(vel_, slot, v);
    for (size_t c = 0; c < attrs_.size(); ++c)
        attrs_[c][slot] = attr_or_null ? attr_or_null[c] : 0.0f;
    for (size_t c = 0; c < states_.size(); ++c)
        states_[c][slot] = state_or_null ? state_or_null[c] : 0.0f;
    alive_[slot] = 1; id_[slot] = next_id_++; age_[slot] = 0;
    dep_dist_[slot] = 0.0f;
    claim_of_[slot] = UINT32_MAX;
    ++alive_n_;
    born_.push_back(slot);
    deposit(p, v);                  // spawn point is wood from tick zero
    return slot;
}

void Sim::kill(uint32_t slot) {
    if (slot < slot_count() && alive_[slot]) kill_slot(slot);
}

// Unconditional kill (callers check liveness). Releases any attractor claim so
// the target becomes available again — unless it was already consumed, in which
// case the claim flag is left alone because the attractor is gone either way.
// Position, velocity and attributes are intentionally NOT cleared: the observer
// reads them for the final path vertex at the end of this tick, and the slot is
// fully reinitialized when it is next reused.
void Sim::kill_slot(uint32_t i) {
    alive_[i] = 0; --alive_n_;
    if (claim_of_[i] != UINT32_MAX) {
        if (!attr_consumed_[claim_of_[i]]) attr_claimed_[claim_of_[i]] = 0;
        claim_of_[i] = UINT32_MAX;
    }
    died_.push_back(i);
    free_slots_.push_back(i);
}

// The one piece of `SimConfig` that is mutable after construction. Out-of-range
// indices are ignored silently. Setting a weight to 0 does more than scale the
// output to nothing — `integrate_slot` skips evaluating the field entirely, so
// this is also the way to switch off an expensive neighborhood field.
void Sim::set_field_weight(uint32_t idx, float w) {
    if (idx < cfg_.fields.size()) cfg_.fields[idx].weight = w;
}

// Axial weight fade: 1 below `from`, 0 above `to`, linear between, where the
// coordinate is dot(position, axis) — an UNNORMALIZED projection measured from
// the world origin, not from the emitter. So `from`/`to` are absolute distances
// along `axis` and depend on where the origin sits, and `axis` is not
// normalized here (a non-unit axis rescales both thresholds).
float Sim::fade_mult(const FieldConfig& f, V3 p) const {
    if (!f.fade.enabled) return 1.0f;
    float t = dot(p, f.fade.axis);
    if (t <= f.fade.from) return 1.0f;
    if (t >= f.fade.to) return 0.0f;
    float d = f.fade.to - f.fade.from;
    return d > 1e-8f ? 1.0f - (t - f.fade.from) / d : 0.0f;
}

// Emit this tick's particles. Each emitter carries a fractional accumulator, so
// `rate` below 1 spreads emissions across ticks deterministically rather than
// rounding to zero.
//
// The sampling frame is built from `axis` plus an arbitrary-but-deterministic
// reference vector; disc samples are area-uniform (sqrt of a uniform draw),
// ring samples sit exactly on the rim. Jitter draws a uniform direction and
// then scales it by a second uniform draw, so the added speed is uniform in
// [0, jitter] rather than always `jitter`.
//
// `attr_init`/`state_init` are staged into two scratch vectors sized to the
// sim's ACTUAL channel counts, so an emitter can initialize every channel it
// declares — there is no channel-count ceiling. (There used to be: the staging
// buffers were fixed 16-element stack arrays, and channels past the sixteenth
// silently started at 0.) The staging is per emitter, not per particle: the
// values do not depend on the particle, so a burst of emissions fills the same
// two vectors once. An emitter that declares FEWER init values than the sim has
// channels leaves the rest at 0, which is intended.
//
// On hitting `max_particles` the emitter zeroes its accumulator and stops for
// this tick, so the backlog is discarded rather than bursting once space frees
// up.
void Sim::run_emitters() {
    std::vector<float> attr_init, state_init;
    for (size_t e = 0; e < cfg_.emitters.size(); ++e) {
        const EmitterConfig& em = cfg_.emitters[e];
        emit_acc_[e] += em.rate;
        if (emit_acc_[e] < 1.0f) continue;

        attr_init.assign(attrs_.size(), 0.0f);
        for (size_t c = 0; c < attrs_.size() && c < em.attr_init.size(); ++c)
            attr_init[c] = em.attr_init[c];
        state_init.assign(states_.size(), 0.0f);
        for (size_t c = 0; c < states_.size() && c < em.state_init.size(); ++c)
            state_init[c] = em.state_init[c];

        while (emit_acc_[e] >= 1.0f) {
            emit_acc_[e] -= 1.0f;
            V3 ax = normalize(em.axis);
            V3 ref = std::fabs(ax.y) < 0.9f ? V3{0,1,0} : V3{1,0,0};
            V3 n1 = normalize(cross(ax, ref));
            V3 n2 = cross(ax, n1);
            V3 p = em.center;
            if (em.shape != 0) {
                float a = rng_.range(0.0f, 6.28318530718f);
                float r = (em.shape == 2) ? em.radius
                          : em.radius * std::sqrt(rng_.next_unit());
                p = p + n1 * (std::cos(a) * r) + n2 * (std::sin(a) * r);
            }
            V3 v = ax * em.vel0;
            if (em.jitter > 0.0f) v = v + rng_.unit_sphere() * (em.jitter * rng_.next_unit());
            if (emit_particle(p, v, attr_init.data(), state_init.data()) == UINT32_MAX) {
                emit_acc_[e] = 0;
                break;
            }
        }
    }
}

// Rotate `v` toward `desired` by at most `max_angle` radians (Rodrigues), always
// preserving |v| — this is the steering clamp, and it is why steer fields can
// never change speed. Returns `v` unchanged when either vector is degenerate,
// and snaps straight to `desired * |v|` when the gap is already within the
// limit.
//
// Near-parallel and near-antiparallel inputs leave the rotation axis undefined;
// an arbitrary perpendicular is substituted. For the antiparallel case that
// means the turn direction is arbitrary — deterministic, but not meaningful.
static V3 rotate_toward(V3 v, V3 desired, float max_angle) {
    float sp = length(v);
    if (sp < 1e-8f || length(desired) < 1e-8f) return v;
    V3 vn = v * (1.0f / sp);
    float c = std::fmax(-1.0f, std::fmin(1.0f, dot(vn, desired)));
    float ang = std::acos(c);
    if (ang <= max_angle) return desired * sp;
    V3 axis = cross(vn, desired);
    if (length(axis) < 1e-6f)  // near-parallel/antiparallel: any perpendicular
        axis = cross(vn, std::fabs(vn.y) < 0.9f ? V3{0,1,0} : V3{1,0,0});
    axis = normalize(axis);
    float ca = std::cos(max_angle), sa = std::sin(max_angle);
    V3 r = vn * ca + cross(axis, vn) * sa + axis * (dot(axis, vn) * (1.0f - ca));
    return r * sp;
}

// One particle, one tick. Order:
//   1. accumulate fields — effective weight is
//      `weight * fade_mult * max(state[weight_state], 0)`, and a zero weight
//      skips the field's evaluation entirely (the cheap-disable path);
//      Force-mode results sum into `force`, Steer-mode unit directions sum
//      into `steer`
//   2. bail if Attract killed this slot mid-loop (that is what the `alive_`
//      re-test guards)
//   3. velocity += force * dt
//   4. rotate the velocity toward normalize(steer), clamped to `max_turn_rate`
//   5. relax the speed toward `speed_target` (skipped when it is negative)
//   6. position += velocity * dt
//   7. NaN/Inf guard, deposit odometer, age/`max_age` expiry
//
// Because step 1 sums unit directions and step 4 normalizes the sum, two
// opposing steer fields of equal weight cancel to zero and the heading is left
// untouched — they do not fight to a midpoint, they simply stop steering.
//
// The odometer accumulates `|v| * dt`, i.e. path length, so `deposit_every` is a
// DISTANCE between deposits and not a tick interval. A non-finite position is
// killed rather than clamped, so one bad field can never poison the cloud.
// With `max_age == 0` (unlimited) `age_` still increments; it is simply never
// tested.
void Sim::integrate_slot(uint32_t i) {
    V3 p = read3(pos_, i), v = read3(vel_, i);
    V3 force{0,0,0}, steer{0,0,0};
    for (size_t fi = 0; fi < cfg_.fields.size(); ++fi) {
        const FieldConfig& f = cfg_.fields[fi];
        float w = f.weight * fade_mult(f, p);
        if (f.weight_state >= 0 && (size_t)f.weight_state < states_.size())
            w *= std::max(states_[(size_t)f.weight_state][i], 0.0f);
        if (w == 0.0f) continue;
        if (f.mode == FieldMode::Force) {
            force = force + field_force(*this, f, i) * w;
        } else {
            V3 d = (f.type == FieldType::Attract) ? attract_dir(f, i, p)
                                                  : field_steer_dir(*this, f, i);
            steer = steer + d * w;
        }
    }
    if (!alive_[i]) return;   // attract capture may have killed this slot
    v = v + force * cfg_.dt;
    V3 desired = normalize(steer);
    if (desired.x != 0 || desired.y != 0 || desired.z != 0)
        v = rotate_toward(v, desired, cfg_.max_turn_rate);
    if (cfg_.speed_target >= 0.0f) {
        float sp = length(v);
        float ns = sp + (cfg_.speed_target - sp) * cfg_.speed_relax;
        V3 dirv = sp > 1e-8f ? v * (1.0f / sp) : desired;
        v = dirv * ns;
    }
    p = p + v * cfg_.dt;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        kill_slot(i);           // NaN guard: kill, never propagate
        return;
    }
    write3(pos_, i, p); write3(vel_, i, v);
    dep_dist_[i] += length(v) * cfg_.dt;
    if (dep_dist_[i] >= cfg_.deposit_every) { deposit(p, v); dep_dist_[i] = 0.0f; }
    if (cfg_.max_age > 0 && ++age_[i] >= cfg_.max_age) kill_slot(i);
    else if (cfg_.max_age == 0) ++age_[i];
}

// One tick. See the file header for why the emitter / hash-rebuild / integrate
// / notify order is what it is. `born_` and `died_` are cleared here, which is
// what makes them valid only inside the observer callback at the end of this
// same call.
void Sim::step() {
    born_.clear(); died_.clear();
    ++tick_;
    run_emitters();
    // Live hash holds start-of-tick positions; already-integrated neighbors
    // are seen at their old position this tick. Deterministic and cheap.
    live_hash_.clear();
    for (uint32_t i = 0; i < slot_count(); ++i)
        if (alive_[i]) live_hash_.insert(read3(pos_, i), i);
    for (uint32_t i = 0; i < slot_count(); ++i)
        if (alive_[i]) integrate_slot(i);
    for (auto* o : observers_) o->on_tick(*this, tick_);
}

void Sim::run(uint32_t n) { for (uint32_t k = 0; k < n; ++k) step(); }

} // namespace pf
