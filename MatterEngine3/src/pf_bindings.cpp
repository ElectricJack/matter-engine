#include "pf_bindings.h"
#include "dsl_state.h"
#include "particle_flow.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace dsl {
namespace {

DslState* state_of(JSContext* c) {
    return static_cast<DslState*>(JS_GetContextOpaque(c));
}

double argd(JSContext* c, JSValueConst v) {
    double d = 0; JS_ToFloat64(c, &d, v); return d;
}

} // namespace

// Bake-scoped registry of live sims/recorders. Owned (via shared_ptr<void>)
// by the DslState so handles die with the bake context. Ids are indices;
// nullptr slots would mean "freed" but we never free mid-bake.
struct PfRegistry {
    std::vector<std::unique_ptr<pf::Sim>> sims;
    std::vector<std::unique_ptr<pf::PathRecorder>> recorders;
};

PfRegistry* pf_registry_of(DslState* st) {
    if (!st->pf_registry()) {
        st->set_pf_registry(std::shared_ptr<void>(
            static_cast<void*>(new PfRegistry),
            [](void* p) { delete static_cast<PfRegistry*>(p); }));
    }
    return static_cast<PfRegistry*>(st->pf_registry());
}

namespace {

pf::Sim* sim_of(JSContext* c, DslState* st, JSValueConst idv) {
    int32_t id = -1; JS_ToInt32(c, &id, idv);
    PfRegistry* reg = pf_registry_of(st);
    if (id < 0 || static_cast<size_t>(id) >= reg->sims.size()) {
        st->set_error("particleSim: stale or invalid sim handle");
        return nullptr;
    }
    return reg->sims[static_cast<size_t>(id)].get();
}

pf::PathRecorder* rec_of(JSContext* c, DslState* st, JSValueConst idv) {
    int32_t id = -1; JS_ToInt32(c, &id, idv);
    PfRegistry* reg = pf_registry_of(st);
    if (id < 0 || static_cast<size_t>(id) >= reg->recorders.size()) {
        st->set_error("pathRecorder: stale or invalid recorder handle");
        return nullptr;
    }
    return reg->recorders[static_cast<size_t>(id)].get();
}

// ---- config-object readers (missing key => default) -------------------------
double get_num(JSContext* c, JSValueConst obj, const char* key, double def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    double out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToFloat64(c, &out, v);
    JS_FreeValue(c, v);
    return out;
}

bool get_bool(JSContext* c, JSValueConst obj, const char* key, bool def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    bool out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) out = JS_ToBool(c, v) > 0;
    JS_FreeValue(c, v);
    return out;
}

pf::V3 get_v3(JSContext* c, JSValueConst obj, const char* key, pf::V3 def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    pf::V3 out = def;
    if (JS_IsObject(v)) {
        for (uint32_t i = 0; i < 3; ++i) {
            JSValue e = JS_GetPropertyUint32(c, v, i);
            double d = 0; JS_ToFloat64(c, &d, e); JS_FreeValue(c, e);
            (&out.x)[i] = static_cast<float>(d);
        }
    }
    JS_FreeValue(c, v);
    return out;
}

std::string get_str(JSContext* c, JSValueConst obj, const char* key,
                    const char* def) {
    JSValue v = JS_GetPropertyStr(c, obj, key);
    std::string out = def;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(c, v);
        if (s) { out = s; JS_FreeCString(c, s); }
    }
    JS_FreeValue(c, v);
    return out;
}

bool parse_field(JSContext* c, DslState* st, JSValueConst f,
                 pf::FieldConfig* out) {
    std::string type = get_str(c, f, "type", "");
    if      (type == "bias")     out->type = pf::FieldType::Bias;
    else if (type == "curl")     out->type = pf::FieldType::Curl;
    else if (type == "adhere")   out->type = pf::FieldType::Adhere;
    else if (type == "attract")  out->type = pf::FieldType::Attract;
    else if (type == "separate") out->type = pf::FieldType::Separate;
    else if (type == "drag")     out->type = pf::FieldType::Drag;
    else {
        st->set_error("particleSim: unknown field type '" + type + "'");
        return false;
    }
    std::string mode = get_str(c, f, "mode", "steer");
    out->mode = (mode == "force") ? pf::FieldMode::Force : pf::FieldMode::Steer;
    out->weight         = static_cast<float>(get_num(c, f, "weight", 1.0));
    out->dir            = get_v3(c, f, "dir", {0, 1, 0});
    out->radius         = static_cast<float>(get_num(c, f, "radius", 0.5));
    out->surface_offset = static_cast<float>(get_num(c, f, "surfaceOffset", 0.0));
    out->influence      = static_cast<float>(get_num(c, f, "influence", 1.0));
    out->kill_radius    = static_cast<float>(get_num(c, f, "killRadius", 0.1));
    out->kill_on_consume= get_bool(c, f, "killOnConsume", true);
    out->scale          = static_cast<float>(get_num(c, f, "scale", 1.0));
    out->seed           = static_cast<uint32_t>(get_num(c, f, "seed", 0.0));
    out->k              = static_cast<float>(get_num(c, f, "k", 1.0));
    JSValue fade = JS_GetPropertyStr(c, f, "fade");
    if (JS_IsObject(fade)) {
        out->fade.enabled = true;
        // fade.axis is a V3 in the kernel; accept [x,y,z] array from JS
        std::string ax = get_str(c, fade, "axis", "");
        if (!ax.empty()) {
            // convenience string shorthand: "x"/{1,0,0}, "y"/{0,1,0}, "z"/{0,0,1}
            if      (ax == "x") out->fade.axis = {1, 0, 0};
            else if (ax == "z") out->fade.axis = {0, 0, 1};
            else                out->fade.axis = {0, 1, 0};
        } else {
            out->fade.axis = get_v3(c, fade, "axis", {0, 1, 0});
        }
        out->fade.from = static_cast<float>(get_num(c, fade, "from", 0.0));
        out->fade.to   = static_cast<float>(get_num(c, fade, "to", 1.0));
    }
    JS_FreeValue(c, fade);
    return true;
}

void parse_emitter(JSContext* c, JSValueConst e, pf::EmitterConfig* out) {
    std::string shape = get_str(c, e, "shape", "point");
    out->shape  = (shape == "disc") ? 1 : (shape == "ring") ? 2 : 0;
    out->center = get_v3(c, e, "center", {0, 0, 0});
    out->axis   = get_v3(c, e, "axis", {0, 1, 0});
    out->radius = static_cast<float>(get_num(c, e, "radius", 0.0));
    out->rate   = static_cast<float>(get_num(c, e, "rate", 1.0));
    // vel0 in the kernel is a scalar float (initial speed along axis)
    out->vel0   = static_cast<float>(get_num(c, e, "vel0", 1.0));
    out->jitter = static_cast<float>(get_num(c, e, "jitter", 0.0));
    JSValue ai = JS_GetPropertyStr(c, e, "attrInit");
    if (JS_IsObject(ai)) {
        JSValue len = JS_GetPropertyStr(c, ai, "length");
        uint32_t n = 0; JS_ToUint32(c, &n, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue x = JS_GetPropertyUint32(c, ai, i);
            double d = 0; JS_ToFloat64(c, &d, x); JS_FreeValue(c, x);
            out->attr_init.push_back(static_cast<float>(d));
        }
    }
    JS_FreeValue(c, ai);
}

// __pf_simCreate(cfgObj) -> id
JSValue j_pf_simCreate(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 1 || !JS_IsObject(a[0])) {
        st->set_error("particleSim: config object required");
        return JS_NewInt32(c, -1);
    }
    pf::SimConfig cfg;
    cfg.seed          = static_cast<uint64_t>(get_num(c, a[0], "seed", 1.0));
    cfg.dt            = static_cast<float>(get_num(c, a[0], "dt", 1.0));
    cfg.max_turn_rate = static_cast<float>(get_num(c, a[0], "maxTurnRate", 0.2));
    cfg.speed_target  = static_cast<float>(get_num(c, a[0], "speedTarget", -1.0)); // <0 = off
    cfg.speed_relax   = static_cast<float>(get_num(c, a[0], "speedRelax", 0.1));
    cfg.deposit_every = static_cast<float>(get_num(c, a[0], "depositEvery", 0.05));
    cfg.max_age       = static_cast<uint32_t>(get_num(c, a[0], "maxAge", 0.0));
    cfg.max_particles = static_cast<uint32_t>(get_num(c, a[0], "maxParticles", 4096.0));
    cfg.hash_cell     = static_cast<float>(get_num(c, a[0], "hashCell", 0.25));
    JSValue attrs = JS_GetPropertyStr(c, a[0], "attributes");
    if (JS_IsObject(attrs)) {
        JSValue len = JS_GetPropertyStr(c, attrs, "length");
        uint32_t na = 0; JS_ToUint32(c, &na, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < na; ++i) {
            JSValue s = JS_GetPropertyUint32(c, attrs, i);
            const char* cs = JS_ToCString(c, s);
            if (cs) { cfg.attributes.push_back(cs); JS_FreeCString(c, cs); }
            JS_FreeValue(c, s);
        }
    }
    JS_FreeValue(c, attrs);
    JSValue ems = JS_GetPropertyStr(c, a[0], "emitters");
    if (JS_IsObject(ems)) {
        JSValue len = JS_GetPropertyStr(c, ems, "length");
        uint32_t ne = 0; JS_ToUint32(c, &ne, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < ne; ++i) {
            JSValue e = JS_GetPropertyUint32(c, ems, i);
            pf::EmitterConfig ec; parse_emitter(c, e, &ec);
            cfg.emitters.push_back(std::move(ec));
            JS_FreeValue(c, e);
        }
    }
    JS_FreeValue(c, ems);
    JSValue flds = JS_GetPropertyStr(c, a[0], "fields");
    if (JS_IsObject(flds)) {
        JSValue len = JS_GetPropertyStr(c, flds, "length");
        uint32_t nf = 0; JS_ToUint32(c, &nf, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < nf; ++i) {
            JSValue f = JS_GetPropertyUint32(c, flds, i);
            pf::FieldConfig fc;
            bool ok = parse_field(c, st, f, &fc);
            JS_FreeValue(c, f);
            if (!ok) { JS_FreeValue(c, flds); return JS_NewInt32(c, -1); }
            cfg.fields.push_back(std::move(fc));
        }
    }
    JS_FreeValue(c, flds);
    PfRegistry* reg = pf_registry_of(st);
    reg->sims.push_back(std::make_unique<pf::Sim>(cfg));
    return JS_NewInt32(c, static_cast<int32_t>(reg->sims.size() - 1));
}

// __pf_recorderCreate(minSegment, channelNamesArray) -> id
JSValue j_pf_recorderCreate(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    float min_seg = (n >= 1) ? static_cast<float>(argd(c, a[0])) : 0.0f;
    std::vector<std::string> names;
    if (n >= 2 && JS_IsObject(a[1])) {
        JSValue len = JS_GetPropertyStr(c, a[1], "length");
        uint32_t nn = 0; JS_ToUint32(c, &nn, len); JS_FreeValue(c, len);
        for (uint32_t i = 0; i < nn; ++i) {
            JSValue s = JS_GetPropertyUint32(c, a[1], i);
            const char* cs = JS_ToCString(c, s);
            if (cs) { names.push_back(cs); JS_FreeCString(c, cs); }
            JS_FreeValue(c, s);
        }
    }
    PfRegistry* reg = pf_registry_of(st);
    reg->recorders.push_back(std::make_unique<pf::PathRecorder>(min_seg, names));
    return JS_NewInt32(c, static_cast<int32_t>(reg->recorders.size() - 1));
}

// __pf_attach(simId, recId)
JSValue j_pf_attach(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.attach: (simId, recId) required"); return JS_UNDEFINED; }
    pf::Sim* sim = sim_of(c, st, a[0]);
    pf::PathRecorder* rec = rec_of(c, st, a[1]);
    if (sim && rec) sim->attach(rec);
    return JS_UNDEFINED;
}

// __pf_setAttractors(simId, Float32Array of xyz triplets)
JSValue j_pf_setAttractors(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.setAttractors: (simId, Float32Array) required"); return JS_UNDEFINED; }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_UNDEFINED;
    size_t byte_off = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(c, a[1], &byte_off, &byte_len, &bpe);
    if (JS_IsException(buf) || bpe != 4 ||
        JS_GetTypedArrayType(a[1]) != JS_TYPED_ARRAY_FLOAT32) {
        JS_FreeValue(c, buf);
        st->set_error("pf.setAttractors: expected a Float32Array");
        return JS_UNDEFINED;
    }
    size_t abuf_len = 0;
    uint8_t* raw = JS_GetArrayBuffer(c, &abuf_len, buf);
    JS_FreeValue(c, buf);
    if (!raw) { st->set_error("pf.setAttractors: detached buffer"); return JS_UNDEFINED; }
    const float* f = reinterpret_cast<const float*>(raw + byte_off);
    size_t count = (byte_len / 4) / 3;
    sim->set_attractors(f, count);   // kernel copies; appends count xyz points
    return JS_UNDEFINED;
}

// Wrap a raw sim buffer as a typed array WITHOUT copying. free_func = nullptr:
// QuickJS does not own the memory; we detach the buffer after the callback so
// JS can never touch freed/moved sim storage. Returns {ta, buf} — caller must
// detach buf and free both values.
struct RawView { JSValue ta; JSValue buf; };

RawView raw_view(JSContext* c, void* data, size_t bytes, JSTypedArrayEnum kind) {
    JSValue buf = JS_NewArrayBuffer(c, static_cast<uint8_t*>(data), bytes,
                                    /*free_func*/ nullptr, /*opaque*/ nullptr,
                                    /*is_shared*/ false);
    JSValue argv[3] = {buf, JS_UNDEFINED, JS_UNDEFINED};
    JSValue ta = JS_NewTypedArray(c, 3, argv, kind);
    return {ta, buf};
}

// Build the per-tick view object; collect its buffers for post-callback detach.
JSValue build_tick_view(JSContext* c, pf::Sim* sim, std::vector<JSValue>* bufs) {
    JSValue o = JS_NewObject(c);
    size_t slots = sim->slot_count();
    JS_SetPropertyStr(c, o, "count", JS_NewInt64(c, static_cast<int64_t>(slots)));
    JS_SetPropertyStr(c, o, "tick", JS_NewInt64(c, static_cast<int64_t>(sim->tick())));
    RawView pos = raw_view(c, sim->pos_data(), slots * 3 * sizeof(float),
                           JS_TYPED_ARRAY_FLOAT32);
    JS_SetPropertyStr(c, o, "pos", pos.ta); bufs->push_back(pos.buf);
    RawView vel = raw_view(c, sim->vel_data(), slots * 3 * sizeof(float),
                           JS_TYPED_ARRAY_FLOAT32);
    JS_SetPropertyStr(c, o, "vel", vel.ta); bufs->push_back(vel.buf);
    RawView alv = raw_view(c, sim->alive_data(), slots * sizeof(uint8_t),
                           JS_TYPED_ARRAY_UINT8);
    JS_SetPropertyStr(c, o, "alive", alv.ta); bufs->push_back(alv.buf);
    JSValue attrs = JS_NewObject(c);
    for (uint32_t ch = 0; ch < sim->channel_count(); ++ch) {
        RawView av = raw_view(c, sim->attr_data(ch), slots * sizeof(float),
                              JS_TYPED_ARRAY_FLOAT32);
        JS_SetPropertyStr(c, attrs, sim->config().attributes[ch].c_str(), av.ta);
        bufs->push_back(av.buf);
    }
    JS_SetPropertyStr(c, o, "attrs", attrs);
    return o;
}

// __pf_run(simId, ticks, every?, onTick?) -> ticks actually run.
// Chunk size = min(every, RUN_CHUNK-capped remainder); budget checked per chunk.
constexpr uint32_t RUN_CHUNK = 32;
JSValue j_pf_run(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) { st->set_error("pf.run: (simId, ticks) required"); return JS_NewInt32(c, 0); }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_NewInt32(c, 0);
    uint32_t ticks = static_cast<uint32_t>(argd(c, a[1]));
    uint32_t every = (n >= 3 && !JS_IsUndefined(a[2]))
                         ? static_cast<uint32_t>(argd(c, a[2])) : 0;
    JSValueConst on_tick = (n >= 4 && JS_IsFunction(c, a[3])) ? a[3] : JS_UNDEFINED;
    bool has_cb = every > 0 && !JS_IsUndefined(on_tick);

    uint32_t done = 0;
    while (done < ticks) {
        if (st->budget_exceeded()) {
            st->set_error("pf.run: bake time budget exceeded mid-simulation");
            break;
        }
        uint32_t chunk = std::min(has_cb ? every : RUN_CHUNK, ticks - done);
        sim->run(chunk);
        done += chunk;
        if (has_cb) {
            std::vector<JSValue> bufs;
            JSValue view = build_tick_view(c, sim, &bufs);
            JSValue arg[1] = {view};
            JSValue r = JS_Call(c, on_tick, JS_UNDEFINED, 1, arg);
            for (JSValue b : bufs) { JS_DetachArrayBuffer(c, b); JS_FreeValue(c, b); }
            JS_FreeValue(c, view);
            if (JS_IsException(r)) { JS_FreeValue(c, r); return JS_EXCEPTION; }
            bool stop = JS_IsBool(r) && JS_ToBool(c, r) == 0;
            JS_FreeValue(c, r);
            if (stop) break;
        }
    }
    return JS_NewInt32(c, static_cast<int32_t>(done));
}

// __pf_emit(simId, cfgObj) — one-shot manual emission. cfg reuses emitter keys:
// { center: [x,y,z], axis: [x,y,z], vel0: speed, attrInit: [floats] }.
// vel is constructed as axis * vel0 (matching how the kernel's emitter fires).
JSValue j_pf_emit(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2 || !JS_IsObject(a[1])) { st->set_error("pf.emit: (simId, cfg) required"); return JS_UNDEFINED; }
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_UNDEFINED;
    pf::EmitterConfig ec; parse_emitter(c, a[1], &ec);
    pf::V3 vel = ec.axis * ec.vel0;
    sim->emit_particle(ec.center, vel,
                       ec.attr_init.empty() ? nullptr : ec.attr_init.data());
    return JS_UNDEFINED;
}

JSValue j_pf_kill(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) return JS_UNDEFINED;
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (sim) sim->kill(static_cast<uint32_t>(argd(c, a[1])));
    return JS_UNDEFINED;
}

JSValue j_pf_setFieldWeight(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 3) return JS_UNDEFINED;
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_UNDEFINED;
    uint32_t i = static_cast<uint32_t>(argd(c, a[1]));
    if (i >= sim->field_count()) { st->set_error("pf.setFieldWeight: field index out of range"); return JS_UNDEFINED; }
    sim->set_field_weight(i, static_cast<float>(argd(c, a[2])));
    return JS_UNDEFINED;
}

JSValue j_pf_attractorsRemaining(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    pf::Sim* sim = (n >= 1) ? sim_of(c, st, a[0]) : nullptr;
    return JS_NewInt32(c, sim ? static_cast<int32_t>(sim->attractors_remaining()) : 0);
}

JSValue j_pf_depositedCount(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    pf::Sim* sim = (n >= 1) ? sim_of(c, st, a[0]) : nullptr;
    return JS_NewInt32(c, sim ? static_cast<int32_t>(sim->deposited_count()) : 0);
}

// __pf_surfaceNormal(simId, x, y, z, radius) -> [nx,ny,nz] | null
JSValue j_pf_surfaceNormal(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 5) return JS_NULL;
    pf::Sim* sim = sim_of(c, st, a[0]);
    if (!sim) return JS_NULL;
    pf::V3 p{static_cast<float>(argd(c, a[1])), static_cast<float>(argd(c, a[2])),
             static_cast<float>(argd(c, a[3]))};
    bool ok = false;
    pf::V3 nrm = sim->surface_normal(p, static_cast<float>(argd(c, a[4])), &ok);
    if (!ok) return JS_NULL;
    JSValue arr = JS_NewArray(c);
    JS_SetPropertyUint32(c, arr, 0, JS_NewFloat64(c, nrm.x));
    JS_SetPropertyUint32(c, arr, 1, JS_NewFloat64(c, nrm.y));
    JS_SetPropertyUint32(c, arr, 2, JS_NewFloat64(c, nrm.z));
    return arr;
}

JSValue j_pf_pathCount(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    pf::PathRecorder* rec = (n >= 1) ? rec_of(c, st, a[0]) : nullptr;
    return JS_NewInt32(c, rec ? static_cast<int32_t>(rec->paths().paths.size()) : 0);
}

// Copy a float vector out as a fresh Float32Array (JS owns the copy).
// Pass buf + two JS_UNDEFINED placeholders so js_typed_array_constructor sees
// argc=3 and reads argv[1]==undefined (offset=0) and argv[2]==undefined (len
// computed from byte_length). Passing argc=1 is UB because the constructor
// unconditionally reads argv[1] and argv[2].
JSValue f32_copy(JSContext* c, const std::vector<float>& v) {
    JSValue buf = JS_NewArrayBufferCopy(
        c, reinterpret_cast<const uint8_t*>(v.empty() ? nullptr : v.data()),
        v.size() * sizeof(float));
    if (JS_IsException(buf)) return buf;
    JSValue argv[3] = {buf, JS_UNDEFINED, JS_UNDEFINED};
    JSValue ta = JS_NewTypedArray(c, 3, argv, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(c, buf);
    return ta;
}

// __pf_path(recId, i) -> {particleId, closed, xyz, channels:{name: Float32Array}}
JSValue j_pf_path(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 2) return JS_NULL;
    pf::PathRecorder* rec = rec_of(c, st, a[0]);
    if (!rec) return JS_NULL;
    int32_t i = -1; JS_ToInt32(c, &i, a[1]);
    const pf::PathSet& ps = rec->paths();
    if (i < 0 || static_cast<size_t>(i) >= ps.paths.size()) return JS_NULL;
    const pf::PathSet::Path& p = ps.paths[static_cast<size_t>(i)];
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "particleId", JS_NewInt64(c, p.particle_id));
    JS_SetPropertyStr(c, o, "closed", JS_NewBool(c, p.closed));
    JS_SetPropertyStr(c, o, "xyz", f32_copy(c, p.xyz));
    JSValue ch = JS_NewObject(c);
    for (size_t k = 0; k < ps.channel_names.size() && k < p.channels.size(); ++k)
        JS_SetPropertyStr(c, ch, ps.channel_names[k].c_str(), f32_copy(c, p.channels[k]));
    JS_SetPropertyStr(c, o, "channels", ch);
    return o;
}

// __pf_stampPaths(recId, opts) — feed recorded paths into the OPEN voxel
// session: one tapered cone brush per segment + a sphere per vertex so joints
// are rounded. Radii come from a recorded channel (default 'thickness'),
// floored at minRadius, scaled by radiusScale.
JSValue j_pf_stampPaths(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (st->session() != dsl::Session::Voxels) {
        st->set_error("paths() outside an open voxel session");
        return JS_UNDEFINED;
    }
    if (n < 1) { st->set_error("paths(): recorder handle required"); return JS_UNDEFINED; }
    pf::PathRecorder* rec = rec_of(c, st, a[0]);
    if (!rec) return JS_UNDEFINED;
    JSValueConst opts = (n >= 2 && JS_IsObject(a[1])) ? a[1] : JS_UNDEFINED;

    std::string rch = "thickness";
    double min_r = 0.01, r_scale = 1.0;
    std::vector<int32_t> filter;
    if (!JS_IsUndefined(opts)) {
        rch     = get_str(c, opts, "radiusChannel", rch.c_str());
        min_r   = get_num(c, opts, "minRadius", min_r);
        r_scale = get_num(c, opts, "radiusScale", r_scale);
        JSValue f = JS_GetPropertyStr(c, opts, "filter");
        if (JS_IsObject(f)) {
            JSValue len = JS_GetPropertyStr(c, f, "length");
            uint32_t nf = 0; JS_ToUint32(c, &nf, len); JS_FreeValue(c, len);
            for (uint32_t i = 0; i < nf; ++i) {
                JSValue e = JS_GetPropertyUint32(c, f, i);
                int32_t idx = -1; JS_ToInt32(c, &idx, e); JS_FreeValue(c, e);
                filter.push_back(idx);
            }
        }
        JS_FreeValue(c, f);
    }

    const pf::PathSet& ps = rec->paths();
    int ch = -1;
    for (size_t k = 0; k < ps.channel_names.size(); ++k)
        if (ps.channel_names[k] == rch) { ch = static_cast<int>(k); break; }

    auto stamp_path = [&](const pf::PathSet::Path& p) {
        size_t nv = p.vertex_count();
        if (nv == 0) return;
        auto radius_at = [&](size_t i) -> float {
            float r = (ch >= 0 && i < p.channels[static_cast<size_t>(ch)].size())
                          ? p.channels[static_cast<size_t>(ch)][i] : static_cast<float>(min_r);
            r *= static_cast<float>(r_scale);
            return std::max(r, static_cast<float>(min_r));
        };
        auto vert = [&](size_t i) -> Vector3 {
            return Vector3{p.xyz[3*i], p.xyz[3*i+1], p.xyz[3*i+2]};
        };
        st->sphere(vert(0), radius_at(0), dsl::CsgOp::Union);
        for (size_t i = 1; i < nv; ++i) {
            st->cone(vert(i - 1), vert(i), radius_at(i - 1), radius_at(i), dsl::CsgOp::Union);
            st->sphere(vert(i), radius_at(i), dsl::CsgOp::Union);
        }
    };

    if (filter.empty()) {
        for (const auto& p : ps.paths) stamp_path(p);
    } else {
        for (int32_t idx : filter)
            if (idx >= 0 && static_cast<size_t>(idx) < ps.paths.size())
                stamp_path(ps.paths[static_cast<size_t>(idx)]);
    }
    return JS_UNDEFINED;
}

} // namespace

void install_pf_bindings(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    auto bind = [&](const char* n, JSCFunction* f, int argc) {
        JS_SetPropertyStr(ctx, g, n, JS_NewCFunction(ctx, f, n, argc));
    };
    bind("__pf_simCreate", j_pf_simCreate, 1);
    bind("__pf_recorderCreate", j_pf_recorderCreate, 2);
    bind("__pf_attach", j_pf_attach, 2);
    bind("__pf_setAttractors", j_pf_setAttractors, 2);
    bind("__pf_run", j_pf_run, 4);
    bind("__pf_attractorsRemaining", j_pf_attractorsRemaining, 1);
    bind("__pf_depositedCount", j_pf_depositedCount, 1);
    bind("__pf_surfaceNormal", j_pf_surfaceNormal, 5);
    bind("__pf_pathCount", j_pf_pathCount, 1);
    bind("__pf_path", j_pf_path, 2);
    bind("__pf_emit", j_pf_emit, 2);
    bind("__pf_kill", j_pf_kill, 2);
    bind("__pf_setFieldWeight", j_pf_setFieldWeight, 3);
    bind("__pf_stampPaths", j_pf_stampPaths, 2);
    JS_FreeValue(ctx, g);
}

} // namespace dsl
