// settle_bench.cpp — headless benchmark CLI for the tileset settle step API
// (settle-tick-optimizer.md §II.4, bake-lab-plan.md task 5.4).
//
// Builds a SettlePlan for a named synthetic fixture, runs it via the
// SettleWorld step API (begin_layer/step/layer_converged/end_layer per
// layer, mirroring settle_tileset's run loop exactly so results are
// bit-identical to the batch path), and prints per-layer tick/wall/pose
// telemetry. Supports dumping the final poses to JSON and comparing two
// dumps with the pose-delta metric (tileset_metrics.h) as an acceptance
// gate for settle optimization experiments.
//
// This tool ALWAYS bypasses the settle cache: it builds a SettlePlan and
// drives a SettleWorld directly, never touching settle_cache_load/save, so
// there is no cache to bypass in the first place — it exists to measure
// physics work, not to skip it.
//
// Usage:
//   settle_bench --spec <name> [--set key=value ...]
//                [--dump out.json] [--compare base.json] [--ticks-csv out.csv]
//
// Fixtures ("--spec"): "synthetic" (~64 physics bodies, mirrors the
// tileset_bake_tests fixture) and "synthetic_large" (~320 physics bodies,
// same shape scaled up) — both built from two parts baked into a throwaway
// cache sandbox at startup (Pebble = voxel sphere, Twig = line capsule).
// Real-world-script fixtures (evaluating an example world's tileset via
// ScriptHost::eval_tileset) are OUT of scope for this task — the eval path
// needs a schemas sandbox; noted as a follow-up.
//
// Exit codes: 0 success (or --compare PASS), 1 --compare FAIL or a build/
// settle failure, 2 usage error (bad args, unknown --set key, or a
// --compare header spec mismatch).

#include "part_graph.h"      // ScriptHost, FileModuleResolver, HostBaker, PartGraph
#include "tileset_bake.h"    // SettlePlan, build_settle_plan, SettledTorus
#include "tileset_metrics.h" // compare_settled, PoseMetricMap, metric_info_of

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using tileset::Pose;

// ---------------------------------------------------------------------------
// Small sandbox helpers (mirrors tileset_bake_tests.cpp).
// ---------------------------------------------------------------------------

static bool write_text_file(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << body;
    return f.good();
}

static void remove_tree(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

static const char* kPebbleJs = R"JS(
class Pebble extends Part {
  static params = { seed: 0 };
  build(p) {
    this.fill(1);
    this.beginVoxels(0.01);
    this.sphere([0, 0, 0], 0.05);
    this.endVoxels();
  }
}
)JS";

static const char* kTwigJs = R"JS(
class Twig extends Part {
  build(p) {
    this.fill(2);
    this.line([-0.1, 0, 0], [0.1, 0, 0], 0.015, 0.015);
  }
}
)JS";

// ---------------------------------------------------------------------------
// Fixture specs.
// ---------------------------------------------------------------------------

// "synthetic": adapted directly from tileset_bake_tests.cpp's make_spec — one
// shared drop, a non-physics pebble layer, a physics twig layer (strips +
// interior + one sync group). ~64 physics bodies total (16 drops + 48 twigs).
//
// "synthetic_large": same shape scaled up — more drops, more interior
// instances per tile, and a third physics layer (pebbles) so the fixture
// mixes collider types (sphere-ish + capsule) across ~320 physics bodies.
static tileset::TilesetSpec make_fixture_spec(uint64_t pebble_hash, uint64_t twig_hash,
                                              bool large) {
    tileset::TilesetSpec s;
    s.tile_called = true;
    s.cfg.size = 2.0f; s.cfg.seed = 99;
    s.base.n = 64; s.base.cell = 2.0f / 64.0f; s.base.material = 1;
    s.base.heights.assign(64 * 64, 0.0f); s.base.set = true;

    // Shared drop(s) (twig): 1 for synthetic, 4 for synthetic_large.
    const int n_drops = large ? 4 : 1;
    for (int d = 0; d < n_drops; ++d) {
        tileset::DropChildRec dr{}; dr.child_hash = twig_hash;
        float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        std::memcpy(dr.transform, I, sizeof I);
        dr.transform[3]  = 0.3f + 0.15f * d;  // TX
        dr.transform[7]  = 0.2f;              // TY
        dr.transform[11] = 0.5f + 0.1f * d;   // TZ
        s.drops.push_back(dr);
    }

    // Layer 0: pebbles, non-physics (snapped to the base).
    tileset::LayerSpec l0; l0.module = "Pebble"; l0.density = 10.0f;
    l0.physics = false; l0.embed = 0.5f;
    const int pebble_per_tile = large ? 8 : 3;
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < pebble_per_tile; ++i) {
            tileset::Placement p{}; p.child_hash = pebble_hash;
            p.pos[0] = 0.1f + 0.15f * i; p.pos[2] = 0.15f + 0.1f * i; p.scale = 1.0f;
            p.quat[3] = 1.0f;
            l0.interior[t].push_back(p);
        }
    s.layers.push_back(l0);

    // Layer 1: twigs, physics — strips (one sync group) + interior.
    tileset::LayerSpec l1; l1.module = "Twig"; l1.density = 4.0f; l1.physics = true;
    for (int o = 0; o < 2; ++o)
        for (int c = 0; c < 2; ++c) {
            tileset::Placement p{}; p.child_hash = twig_hash;
            p.pos[0] = (o == 0) ? 0.05f : 1.0f;
            p.pos[1] = 0.2f;
            p.pos[2] = (o == 0) ? 1.0f : 0.05f;
            p.scale = 1.0f; p.quat[3] = 1.0f;
            l1.strip[o][c].push_back(p);
        }
    const int twig_interior_per_tile = large ? 8 : 1;
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < twig_interior_per_tile; ++i) {
            tileset::Placement p{}; p.child_hash = twig_hash;
            p.pos[0] = 1.0f + 0.05f * i; p.pos[1] = 0.25f; p.pos[2] = 1.2f + 0.05f * i;
            p.scale = 1.0f; p.quat[3] = 1.0f;
            l1.interior[t].push_back(p);
        }
    s.layers.push_back(l1);

    if (large) {
        // Layer 2: pebbles, physics — mixes a second collider type into the
        // physics pass (pebble fits sphere/hull vs. twig's capsule).
        tileset::LayerSpec l2; l2.module = "Pebble"; l2.density = 6.0f; l2.physics = true;
        for (int t = 0; t < 16; ++t)
            for (int i = 0; i < 6; ++i) {
                tileset::Placement p{}; p.child_hash = pebble_hash;
                p.pos[0] = 0.2f + 0.12f * i; p.pos[1] = 0.3f + 0.05f * i;
                p.pos[2] = 0.3f + 0.1f * i;
                p.scale = 1.0f; p.quat[3] = 1.0f;
                l2.interior[t].push_back(p);
            }
        s.layers.push_back(l2);
    }
    return s;
}

static bool build_fixture(const std::string& name, uint64_t pebble_hash, uint64_t twig_hash,
                          tileset::TilesetSpec& out) {
    if (name == "synthetic")       { out = make_fixture_spec(pebble_hash, twig_hash, false); return true; }
    if (name == "synthetic_large") { out = make_fixture_spec(pebble_hash, twig_hash, true);  return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Layer run (step API), mirroring settle_tileset / tileset_bake.cpp's
// settle_layer loop exactly: begin_layer(spawns); while (sim_time <
// max_sim_time && !layer_converged()) step(); end_layer().
// ---------------------------------------------------------------------------

struct TickRow {
    std::string layer;
    int   tick = 0;
    float sim_time = 0.0f;
    int   layer_awake = 0, total_awake = 0;
    float max_lin_vel = 0.0f, max_ang_vel = 0.0f;
    int   wake_events = 0;
    float step_ms = 0.0f;
};

struct LayerRunResult {
    std::string label;
    size_t bodies = 0;
    int    ticks = 0;
    double wall_ms = 0.0;
    bool   converged = false;
    int    awake_tail = 0;
    int    first_contact_tick = -1;
};

static LayerRunResult run_layer(tileset::SettleWorld& world,
                                const std::vector<tileset::BodySpawn>& spawns,
                                const tileset::SettleParams& params,
                                const std::string& label,
                                std::vector<TickRow>* ticks_out) {
    LayerRunResult r;
    r.label = label;
    r.bodies = spawns.size();
    if (spawns.empty()) {
        r.converged = true;   // matches settle_tileset's trivial-result branch
        return r;
    }

    const auto t0 = std::chrono::steady_clock::now();
    world.begin_layer(spawns);
    float sim_time = 0.0f;
    while (sim_time < params.max_sim_time && !world.layer_converged()) {
        tileset::TickStats ts = world.step();
        sim_time = ts.sim_time;
        r.ticks = ts.tick + 1;
        if (r.first_contact_tick < 0 && ts.first_contact) r.first_contact_tick = ts.tick;
        if (ticks_out) {
            TickRow row;
            row.layer = label;
            row.tick = ts.tick; row.sim_time = ts.sim_time;
            row.layer_awake = ts.layer_awake; row.total_awake = ts.total_awake;
            row.max_lin_vel = ts.max_lin_vel; row.max_ang_vel = ts.max_ang_vel;
            row.wake_events = ts.wake_events; row.step_ms = ts.step_ms;
            ticks_out->push_back(row);
        }
    }
    tileset::LayerResult lr = world.end_layer();
    const auto t1 = std::chrono::steady_clock::now();
    r.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.converged = lr.converged;
    r.awake_tail = lr.awake_count;
    return r;
}

static void print_layer_line(const LayerRunResult& r) {
    std::printf("layer %-12s bodies=%-5zu ticks=%-5d wall_ms=%-9.3f converged=%d awake_tail=%-4d first_contact_tick=%d\n",
               r.label.c_str(), r.bodies, r.ticks, r.wall_ms, r.converged ? 1 : 0,
               r.awake_tail, r.first_contact_tick);
}

// ---------------------------------------------------------------------------
// Assemble the final SettledTorus (mirrors settle_tileset's assembly tail:
// tileset_bake.cpp, "Assemble output instances in the correct order").
// ---------------------------------------------------------------------------

static void assemble_torus(const tileset::SettlePlan& plan,
                           const std::vector<Pose>& phys_poses,
                           const tileset::TilesetSpec& spec,
                           tileset::SettledTorus& out) {
    size_t idx = 0;
    for (const auto& pv : plan.drop_provs) {
        tileset::SettledInstance si;
        si.child_hash = pv.child_hash; si.scale = pv.scale;
        si.pose = phys_poses[idx++]; si.layer = -1;
        out.instances.push_back(si);
    }
    for (const auto& lp : plan.layers) {
        for (const auto& pv : lp.provs) {
            tileset::SettledInstance si;
            si.child_hash = pv.child_hash; si.scale = pv.scale;
            si.pose = phys_poses[idx++]; si.layer = pv.layer;
            out.instances.push_back(si);
        }
        for (const auto& ni : lp.nonphys) {
            tileset::SettledInstance si;
            si.child_hash = ni.child_hash; si.scale = ni.scale;
            si.pose = ni.pose; si.layer = ni.layer;
            out.instances.push_back(si);
        }
    }
    out.cfg = spec.cfg;
    out.base = spec.base;
    out.variant_ranges = spec.variant_ranges;
}

// Build the pose-delta metric map from the plan's collider storage.
//
// PoseMetricMap is keyed per child_hash (unscaled char size), but
// SettlePlan::colliders is keyed per (child_hash, override, scale) — a
// single child can appear multiple times if placed at different scales or
// with different collider overrides. This fixture set never does that, but
// if a future fixture does, this takes the first entry encountered per
// child_hash (std::map order: child_hash, then override_str, then scale) and
// ignores the rest — a documented approximation, not a correctness bug: the
// metric map only affects the *comparison* pass, not the physics.
static tileset::PoseMetricMap build_metric_map(const tileset::SettlePlan& plan) {
    tileset::PoseMetricMap m;
    for (const auto& kv : plan.colliders) {
        const uint64_t hash = kv.first.child_hash;
        if (m.count(hash)) continue;  // first-wins approximation, see comment above
        tileset::PoseMetricInfo info = tileset::metric_info_of(kv.second);
        // plan.colliders stores collider fits already scaled by scale_fit();
        // PoseMetricMap wants the UNSCALED char size (compare_settled applies
        // each instance's own scale at comparison time).
        const float scale = kv.first.scale;
        if (scale > 1e-9f) info.char_size /= scale;
        m[hash] = info;
    }
    return m;
}

// ---------------------------------------------------------------------------
// Minimal JSON writer/reader for the dump format. Hand-rolled and tailored
// to exactly the schema this tool writes (no general interop requirement) —
// see the --dump/--compare doc comment above for the shape.
// ---------------------------------------------------------------------------

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

static std::string hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llx", (unsigned long long)v);
    return buf;
}

static bool hex_to_u64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    out = std::strtoull(s.c_str(), nullptr, 16);
    return true;
}

struct DumpInstance {
    uint64_t child_hash = 0;
    float    scale = 1.0f;
    Pose     pose{};
    int      layer = -1;
};

struct DumpHeader {
    std::string spec;
    float       cfg_size = 0.0f;
    uint64_t    pose_hash = 0;
};

static void write_dump(const fs::path& path, const std::string& spec_name,
                       const tileset::SettleParams& params, float cfg_size,
                       uint64_t pose_hash, const tileset::SettledTorus& torus) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o << "{\n  \"header\": {\n";
    o << "    \"spec\": \"" << json_escape(spec_name) << "\",\n";
    o << "    \"cfg_size\": " << std::setprecision(9) << cfg_size << ",\n";
    o << "    \"params\": {\n";
    o << "      \"dt\": " << std::setprecision(9) << params.dt << ",\n";
    o << "      \"substeps\": " << params.substeps << ",\n";
    o << "      \"max_sim_time\": " << std::setprecision(9) << params.max_sim_time << ",\n";
    o << "      \"sleep_fraction\": " << std::setprecision(9) << params.sleep_fraction << ",\n";
    o << "      \"sim_scale\": " << std::setprecision(9) << params.sim_scale << ",\n";
    o << "      \"micro_relax_steps\": " << params.micro_relax_steps << "\n";
    o << "    },\n";
    o << "    \"pose_hash\": \"" << hex64(pose_hash) << "\"\n";
    o << "  },\n  \"instances\": [\n";
    for (size_t i = 0; i < torus.instances.size(); ++i) {
        const auto& si = torus.instances[i];
        o << "    { \"child_hash\": \"" << hex64(si.child_hash) << "\", "
          << "\"scale\": " << std::setprecision(9) << si.scale << ", "
          << "\"pose\": { \"px\": " << std::setprecision(9) << si.pose.px
          << ", \"py\": " << std::setprecision(9) << si.pose.py
          << ", \"pz\": " << std::setprecision(9) << si.pose.pz
          << ", \"qx\": " << std::setprecision(9) << si.pose.qx
          << ", \"qy\": " << std::setprecision(9) << si.pose.qy
          << ", \"qz\": " << std::setprecision(9) << si.pose.qz
          << ", \"qw\": " << std::setprecision(9) << si.pose.qw << " }, "
          << "\"layer\": " << si.layer << " }"
          << (i + 1 < torus.instances.size() ? ",\n" : "\n");
    }
    o << "  ]\n}\n";
    write_text_file(path, o.str());
}

// Tiny generic JSON value parser (objects/arrays/strings/numbers/bool/null)
// used only to read back what write_dump() produces.
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;
    const JsonValue* get(const std::string& key) const {
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool parse(JsonValue& out) { skip_ws(); return parse_value(out); }
private:
    const std::string& s_;
    size_t i_ = 0;
    void skip_ws() { while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) ++i_; }
    bool parse_value(JsonValue& v) {
        skip_ws();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') return parse_object(v);
        if (c == '[') return parse_array(v);
        if (c == '"') return parse_string(v);
        if (c == 't' || c == 'f') return parse_bool(v);
        if (c == 'n') { i_ += 4; v.type = JsonValue::Null; return true; }
        return parse_number(v);
    }
    bool parse_object(JsonValue& v) {
        v.type = JsonValue::Object; ++i_; skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            JsonValue key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            ++i_;
            JsonValue val;
            if (!parse_value(val)) return false;
            v.obj.emplace_back(key.str, val);
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; break; }
            return false;
        }
        return true;
    }
    bool parse_array(JsonValue& v) {
        v.type = JsonValue::Array; ++i_; skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        while (true) {
            JsonValue val;
            if (!parse_value(val)) return false;
            v.arr.push_back(val);
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; break; }
            return false;
        }
        return true;
    }
    bool parse_string(JsonValue& v) {
        skip_ws();
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    default:  out += e;    break;
                }
            } else out += c;
        }
        if (i_ >= s_.size()) return false;
        ++i_;
        v.type = JsonValue::String; v.str = out;
        return true;
    }
    bool parse_bool(JsonValue& v) {
        if (s_.compare(i_, 4, "true") == 0)  { v.type = JsonValue::Bool; v.b = true;  i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { v.type = JsonValue::Bool; v.b = false; i_ += 5; return true; }
        return false;
    }
    bool parse_number(JsonValue& v) {
        size_t start = i_;
        while (i_ < s_.size() &&
               (std::isdigit((unsigned char)s_[i_]) || s_[i_] == '-' || s_[i_] == '+' ||
                s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
            ++i_;
        if (i_ == start) return false;
        v.type = JsonValue::Number;
        v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

static bool read_dump(const fs::path& path, DumpHeader& header, std::vector<DumpInstance>& out,
                      std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path.string(); return false; }
    std::ostringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();   // named lvalue: JsonParser stores a reference to it
    JsonValue root;
    JsonParser parser(text);
    if (!parser.parse(root) || root.type != JsonValue::Object) {
        err = "malformed JSON in " + path.string(); return false;
    }
    const JsonValue* hdr = root.get("header");
    if (!hdr) { err = "missing header"; return false; }
    const JsonValue* spec = hdr->get("spec");
    const JsonValue* cfg_size = hdr->get("cfg_size");
    const JsonValue* pose_hash = hdr->get("pose_hash");
    if (!spec || !cfg_size || !pose_hash) { err = "malformed header"; return false; }
    header.spec = spec->str;
    header.cfg_size = (float)cfg_size->num;
    if (!hex_to_u64(pose_hash->str, header.pose_hash)) { err = "bad pose_hash"; return false; }

    const JsonValue* insts = root.get("instances");
    if (!insts || insts->type != JsonValue::Array) { err = "missing instances"; return false; }
    out.clear();
    out.reserve(insts->arr.size());
    for (const auto& iv : insts->arr) {
        DumpInstance di;
        const JsonValue* ch = iv.get("child_hash");
        const JsonValue* sc = iv.get("scale");
        const JsonValue* po = iv.get("pose");
        const JsonValue* ly = iv.get("layer");
        if (!ch || !sc || !po || !ly) { err = "malformed instance"; return false; }
        if (!hex_to_u64(ch->str, di.child_hash)) { err = "bad child_hash"; return false; }
        di.scale = (float)sc->num;
        di.layer = (int)ly->num;
        const JsonValue* px = po->get("px"); const JsonValue* py = po->get("py");
        const JsonValue* pz = po->get("pz");
        const JsonValue* qx = po->get("qx"); const JsonValue* qy = po->get("qy");
        const JsonValue* qz = po->get("qz"); const JsonValue* qw = po->get("qw");
        if (!px || !py || !pz || !qx || !qy || !qz || !qw) { err = "malformed pose"; return false; }
        di.pose.px = (float)px->num; di.pose.py = (float)py->num; di.pose.pz = (float)pz->num;
        di.pose.qx = (float)qx->num; di.pose.qy = (float)qy->num; di.pose.qz = (float)qz->num;
        di.pose.qw = (float)qw->num;
        out.push_back(di);
    }
    return true;
}

static void write_ticks_csv(const fs::path& path, const std::vector<TickRow>& rows) {
    std::ostringstream o;
    o << "layer,tick,sim_time,layer_awake,total_awake,max_lin_vel,max_ang_vel,wake_events,step_ms\n";
    o.setf(std::ios::fixed);
    for (const auto& r : rows) {
        o << r.layer << ',' << r.tick << ',' << std::setprecision(6) << r.sim_time << ','
          << r.layer_awake << ',' << r.total_awake << ',' << std::setprecision(6) << r.max_lin_vel
          << ',' << std::setprecision(6) << r.max_ang_vel << ',' << r.wake_events << ','
          << std::setprecision(6) << r.step_ms << "\n";
    }
    write_text_file(path, o.str());
}

// ---------------------------------------------------------------------------
// CLI arg parsing.
// ---------------------------------------------------------------------------

static void print_usage() {
    std::fprintf(stderr,
        "usage: settle_bench --spec <synthetic|synthetic_large>\n"
        "                     [--set key=value ...]   (dt, substeps, max_sim_time,\n"
        "                                               sleep_fraction, sim_scale,\n"
        "                                               micro_relax_steps)\n"
        "                     [--dump out.json]\n"
        "                     [--compare base.json]\n"
        "                     [--ticks-csv out.csv]\n");
}

static bool apply_set(tileset::SettleParams& params, const std::string& kv, std::string& err) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos) { err = "--set expects key=value, got '" + kv + "'"; return false; }
    std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
    char* endp = nullptr;
    if (key == "dt") { params.dt = std::strtof(val.c_str(), &endp); }
    else if (key == "substeps") { params.substeps = (int)std::strtol(val.c_str(), &endp, 10); }
    else if (key == "max_sim_time") { params.max_sim_time = std::strtof(val.c_str(), &endp); }
    else if (key == "sleep_fraction") { params.sleep_fraction = std::strtof(val.c_str(), &endp); }
    else if (key == "sim_scale") { params.sim_scale = std::strtof(val.c_str(), &endp); }
    else if (key == "micro_relax_steps") { params.micro_relax_steps = (int)std::strtol(val.c_str(), &endp, 10); }
    else { err = "unknown --set key '" + key + "'"; return false; }
    if (endp == val.c_str() || (endp && *endp != '\0')) {
        err = "--set " + key + ": cannot parse '" + val + "'";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::string spec_name, dump_path, compare_path, ticks_csv_path;
    std::vector<std::string> set_args;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_val = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", flag);
                print_usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--spec") spec_name = need_val("--spec");
        else if (a == "--set") set_args.push_back(need_val("--set"));
        else if (a == "--dump") dump_path = need_val("--dump");
        else if (a == "--compare") compare_path = need_val("--compare");
        else if (a == "--ticks-csv") ticks_csv_path = need_val("--ticks-csv");
        else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            print_usage();
            return 2;
        }
    }
    if (spec_name.empty()) {
        std::fprintf(stderr, "error: --spec is required\n");
        print_usage();
        return 2;
    }

    tileset::SettleParams params{};
    for (const auto& kv : set_args) {
        std::string err;
        if (!apply_set(params, kv, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            print_usage();
            return 2;
        }
    }

    // ---- Sandbox: bake Pebble + Twig once (mirrors tileset_bake_tests.cpp) ----
    namespace pg = part_graph;
    const fs::path prevcwd = fs::current_path();
    const fs::path root = fs::temp_directory_path() / "me3_settle_bench";
    remove_tree(root);
    fs::create_directories(root / "objects");
    fs::create_directories(root / "parts");
    write_text_file(root / "objects" / "Pebble.js", kPebbleJs);
    write_text_file(root / "objects" / "Twig.js", kTwigJs);

    std::error_code ec;
    fs::current_path(root, ec);
    if (ec.value() != 0) {
        std::fprintf(stderr, "error: cannot chdir into sandbox %s\n", root.string().c_str());
        return 1;
    }

    int rc = 0;
    {
        script_host::ScriptHost host;
        pg::FileModuleResolver resolver(host, "objects");
        pg::HostBaker baker(host, ".");
        pg::PartGraph graph(resolver, baker);

        pg::InstallResult pebble_ir = graph.install({ pg::ChildRequest{ "Pebble", pg::Params{} } });
        pg::InstallResult twig_ir   = graph.install({ pg::ChildRequest{ "Twig", pg::Params{} } });
        if (!pebble_ir.ok || pebble_ir.root_hashes.empty() ||
            !twig_ir.ok || twig_ir.root_hashes.empty()) {
            std::fprintf(stderr, "error: fixture part bake failed (pebble: %s) (twig: %s)\n",
                         pebble_ir.error.c_str(), twig_ir.error.c_str());
            rc = 1;
        } else {
            uint64_t pebble_hash = pebble_ir.root_hashes[0];
            uint64_t twig_hash   = twig_ir.root_hashes[0];

            tileset::TilesetSpec spec;
            if (!build_fixture(spec_name, pebble_hash, twig_hash, spec)) {
                std::fprintf(stderr, "error: unknown --spec '%s'\n", spec_name.c_str());
                print_usage();
                rc = 2;
            } else {
                tileset::BakeInputs in{ root.string() };
                tileset::SettlePlan plan;
                std::string err;
                if (!tileset::build_settle_plan(spec, in, plan, err)) {
                    std::fprintf(stderr, "error: build_settle_plan failed: %s\n", err.c_str());
                    rc = 1;
                } else {
                    // ---- Run via the step API (bypasses the settle cache entirely:
                    // this drives a SettleWorld directly from a freshly built plan,
                    // never calling settle_cache_load/save). ----
                    tileset::SettleWorld world(plan.torus_size, plan.hf, params);
                    for (const auto& frames : plan.sync_group_frames)
                        world.add_sync_group(frames);

                    std::vector<TickRow> tick_rows;
                    std::vector<TickRow>* ticks_out = ticks_csv_path.empty() ? nullptr : &tick_rows;

                    std::vector<LayerRunResult> layer_results;
                    layer_results.push_back(run_layer(world, plan.drop_spawns, params, "drops", ticks_out));
                    for (const auto& lp : plan.layers)
                        layer_results.push_back(run_layer(world, lp.spawns, params, lp.module, ticks_out));

                    world.finalize();
                    const std::vector<Pose>& phys_poses = world.poses();
                    const uint64_t pose_hash = world.pose_hash();

                    tileset::SettledTorus candidate;
                    assemble_torus(plan, phys_poses, spec, candidate);

                    size_t total_bodies = 0; int total_ticks = 0; double total_wall_ms = 0.0;
                    bool all_converged = true;
                    for (const auto& r : layer_results) {
                        print_layer_line(r);
                        total_bodies += r.bodies; total_ticks += r.ticks; total_wall_ms += r.wall_ms;
                        if (!r.converged) all_converged = false;
                    }
                    std::printf("total  bodies=%zu ticks=%d wall_ms=%.3f converged=%d\n",
                               total_bodies, total_ticks, total_wall_ms, all_converged ? 1 : 0);
                    std::printf("pose_hash=%s\n", hex64(pose_hash).c_str());

                    // dump_path/ticks_csv_path/compare_path are relative to the
                    // caller's original cwd; we chdir'd into the sandbox above.
                    if (!dump_path.empty())
                        write_dump(fs::path(prevcwd) / dump_path, spec_name, params, spec.cfg.size,
                                  pose_hash, candidate);
                    if (!ticks_csv_path.empty())
                        write_ticks_csv(fs::path(prevcwd) / ticks_csv_path, tick_rows);

                    if (!compare_path.empty()) {
                        DumpHeader base_header;
                        std::vector<DumpInstance> base_insts;
                        std::string rerr;
                        if (!read_dump(fs::path(prevcwd) / compare_path, base_header, base_insts, rerr)) {
                            std::fprintf(stderr, "error: --compare: %s\n", rerr.c_str());
                            rc = 2;
                        } else if (base_header.spec != spec_name) {
                            std::fprintf(stderr,
                                        "error: --compare header spec mismatch: dump was built with "
                                        "'%s', this run is '%s'\n",
                                        base_header.spec.c_str(), spec_name.c_str());
                            rc = 2;
                        } else {
                            tileset::SettledTorus baseline;
                            baseline.cfg.size = base_header.cfg_size;
                            for (const auto& di : base_insts) {
                                tileset::SettledInstance si;
                                si.child_hash = di.child_hash; si.scale = di.scale;
                                si.pose = di.pose; si.layer = di.layer;
                                baseline.instances.push_back(si);
                            }
                            tileset::PoseMetricMap metric_map = build_metric_map(plan);
                            tileset::PoseDeltaReport rep =
                                tileset::compare_settled(baseline, candidate, metric_map);
                            std::printf("compare: median=%.6f p95=%.6f max=%.6f p95_orient=%.3f "
                                       "teleports=%d compared=%d skipped=%d\n",
                                       rep.median, rep.p95, rep.max, rep.p95_orient_deg,
                                       rep.teleports, rep.compared, rep.skipped);
                            std::printf("GATE: %s\n", rep.pass ? "PASS" : "FAIL");
                            if (rc == 0) rc = rep.pass ? 0 : 1;
                        }
                    }
                }
            }
        }
    }

    fs::current_path(prevcwd, ec);
    remove_tree(root);
    return rc;
}
