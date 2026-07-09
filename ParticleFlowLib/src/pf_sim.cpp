#include "particle_flow.h"
#include <algorithm>
#include <cmath>

namespace pf {

// Implemented in pf_fields.cpp. Bias/Drag land in Task 2; Curl/Adhere/Separate
// in Task 3 (until then they contribute zero, which is valid field behavior).
V3 field_steer_dir(const Sim& s, const FieldConfig& f, uint32_t slot);
V3 field_force(const Sim& s, const FieldConfig& f, uint32_t slot);

static inline V3 read3(const std::vector<float>& a, uint32_t i) {
    return {a[3*i], a[3*i+1], a[3*i+2]};
}
static inline void write3(std::vector<float>& a, uint32_t i, V3 v) {
    a[3*i] = v.x; a[3*i+1] = v.y; a[3*i+2] = v.z;
}

static float auto_cell(const SimConfig& c) {
    float r = 0.0f;
    for (const auto& f : c.fields) {
        if (f.type == FieldType::Adhere || f.type == FieldType::Separate)
            r = std::max(r, f.radius);
        if (f.type == FieldType::Attract)
            r = std::max(r, f.influence);
    }
    return r > 1e-6f ? r : 1.0f;
}

Sim::Sim(SimConfig cfg)
    : cfg_(std::move(cfg)), rng_(cfg_.seed),
      dep_hash_(cfg_.hash_cell > 0 ? cfg_.hash_cell : auto_cell(cfg_)),
      live_hash_(cfg_.hash_cell > 0 ? cfg_.hash_cell : auto_cell(cfg_)) {
    attrs_.resize(cfg_.attributes.size());
    emit_acc_.assign(cfg_.emitters.size(), 0.0f);
    // pre-reserve to capacity: onTick views hold raw pointers; growth must never reallocate
    pos_.reserve(3 * cfg_.max_particles);
    vel_.reserve(3 * cfg_.max_particles);
    alive_.reserve(cfg_.max_particles);
    id_.reserve(cfg_.max_particles);
    age_.reserve(cfg_.max_particles);
    dep_dist_.reserve(cfg_.max_particles);
    for (auto& ch : attrs_) ch.reserve(cfg_.max_particles);
}

void Sim::attach(ITickObserver* o) { observers_.push_back(o); }

void Sim::set_attractors(const float* xyz, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        attractors_.push_back({xyz[3*i], xyz[3*i+1], xyz[3*i+2]});
        attr_consumed_.push_back(0);
    }
    attr_remaining_ += (uint32_t)n;
}

int Sim::channel_index(const std::string& name) const {
    for (size_t i = 0; i < cfg_.attributes.size(); ++i)
        if (cfg_.attributes[i] == name) return (int)i;
    return -1;
}

void Sim::deposit(V3 p) {
    dep_hash_.insert(p, (uint32_t)deposited_pts_.size());
    deposited_pts_.push_back(p);
}

uint32_t Sim::emit_particle(V3 p, V3 v, const float* attr_or_null) {
    if (alive_n_ >= cfg_.max_particles) return UINT32_MAX;
    uint32_t slot;
    if (!free_slots_.empty()) { slot = free_slots_.back(); free_slots_.pop_back(); }
    else {
        slot = slot_count();
        pos_.resize(pos_.size() + 3); vel_.resize(vel_.size() + 3);
        for (auto& ch : attrs_) ch.push_back(0.0f);
        alive_.push_back(0); id_.push_back(0); age_.push_back(0);
        dep_dist_.push_back(0.0f);
    }
    write3(pos_, slot, p); write3(vel_, slot, v);
    for (size_t c = 0; c < attrs_.size(); ++c)
        attrs_[c][slot] = attr_or_null ? attr_or_null[c] : 0.0f;
    alive_[slot] = 1; id_[slot] = next_id_++; age_[slot] = 0;
    dep_dist_[slot] = 0.0f;
    ++alive_n_;
    born_.push_back(slot);
    deposit(p);                     // spawn point is wood from tick zero
    return slot;
}

void Sim::kill(uint32_t slot) {
    if (slot < slot_count() && alive_[slot]) kill_slot(slot);
}

void Sim::kill_slot(uint32_t i) {
    alive_[i] = 0; --alive_n_;
    died_.push_back(i);
    free_slots_.push_back(i);
}

void Sim::set_field_weight(uint32_t idx, float w) {
    if (idx < cfg_.fields.size()) cfg_.fields[idx].weight = w;
}

float Sim::fade_mult(const FieldConfig& f, V3 p) const {
    if (!f.fade.enabled) return 1.0f;
    float t = dot(p, f.fade.axis);
    if (t <= f.fade.from) return 1.0f;
    if (t >= f.fade.to) return 0.0f;
    float d = f.fade.to - f.fade.from;
    return d > 1e-8f ? 1.0f - (t - f.fade.from) / d : 0.0f;
}

void Sim::run_emitters() {
    for (size_t e = 0; e < cfg_.emitters.size(); ++e) {
        const EmitterConfig& em = cfg_.emitters[e];
        emit_acc_[e] += em.rate;
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
            float attrs[16] = {0};
            size_t nc = std::min(attrs_.size(), (size_t)16);
            for (size_t c = 0; c < nc; ++c)
                attrs[c] = c < em.attr_init.size() ? em.attr_init[c] : 0.0f;
            if (emit_particle(p, v, attrs) == UINT32_MAX) { emit_acc_[e] = 0; break; }
        }
    }
}

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

void Sim::integrate_slot(uint32_t i) {
    V3 p = read3(pos_, i), v = read3(vel_, i);
    V3 force{0,0,0}, steer{0,0,0};
    for (size_t fi = 0; fi < cfg_.fields.size(); ++fi) {
        const FieldConfig& f = cfg_.fields[fi];
        float w = f.weight * fade_mult(f, p);
        if (w == 0.0f) continue;
        if (f.mode == FieldMode::Force) {
            force = force + field_force(*this, f, i) * w;
        } else {
            V3 d = (f.type == FieldType::Attract) ? attract_dir(i, p)
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
    if (dep_dist_[i] >= cfg_.deposit_every) { deposit(p); dep_dist_[i] = 0.0f; }
    if (cfg_.max_age > 0 && ++age_[i] >= cfg_.max_age) kill_slot(i);
    else if (cfg_.max_age == 0) ++age_[i];
}

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
