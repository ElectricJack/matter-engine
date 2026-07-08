#include "part_graph.h"
#include "part_asset_v2.h"   // SP-1 (MatterEngine3, via -I../include): compute_resolved_hash,
                            //   cache_path_resolved; pulls in v1 part_asset.h for fnv1a64
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace part_graph {

std::string serialize_params(const Params& params) {
    std::string out;
    for (const auto& kv : params) {          // std::map iterates in sorted key order
        out += kv.first;
        out += '=';
        const ParamValue& v = kv.second;
        switch (v.kind) {
            case ParamValue::Kind::Number: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", v.num);
                out += buf;
                break;
            }
            case ParamValue::Kind::Bool:
                out += (v.boolean ? "true" : "false");
                break;
            case ParamValue::Kind::Str:
                out += v.str;
                break;
        }
        out += ';';
    }
    return out;
}

// Params -> JSON object string (numbers via %.17g, strings quoted). This is the
// canonical form placeChild's JSON.stringify must match for parametric child
// selection, and the form the host re-canonicalizes for a child's own resolved
// hash. Always compiled (no host dependency) so install() can key child
// placements by it regardless of the script-host build flag.
std::string params_to_json(const Params& params) {
    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto& kv : params) {          // Params is an ordered map; host re-sorts
        if (!first) os << ','; first = false;
        os << '"' << kv.first << "\":";
        switch (kv.second.kind) {
            case ParamValue::Kind::Number: {
                char buf[32]; std::snprintf(buf, sizeof buf, "%.17g", kv.second.num);
                os << buf; break;
            }
            case ParamValue::Kind::Bool:
                os << (kv.second.boolean ? "true" : "false"); break;
            case ParamValue::Kind::Str:
                os << '"' << kv.second.str << '"'; break;  // SP-3 v1 params have no quotes/escapes
        }
    }
    os << '}';
    return os.str();
}

PartGraph::PartGraph(ModuleResolver& resolver, Baker& baker)
    : resolver_(resolver), baker_(baker) {}

namespace {

struct InternalNode {
    uint64_t              memo_key = 0;       // fnv1a64(source) folded with canonical params
    uint64_t              resolved_hash = 0;
    std::string           module;
    std::string           source;
    Params                params;
    std::vector<uint64_t> child_hashes;       // direct children (for SP-1 fold, sorted)
    std::vector<uint64_t> child_keys;         // direct children memo keys (topo edges)
    std::vector<std::string> child_modules;   // direct children's module names (parallel to child_hashes)
    std::vector<std::string> child_params;    // direct children's canonical params JSON (parallel to child_hashes)
};

// Memo key = fnv1a64(source) combined with fnv1a64(canonical_params). Identity is the
// SOURCE HASH (not the path), per the planning decision, so a renamed identical script
// is one node.
uint64_t memo_key_of(const std::string& source, const std::string& canon_params) {
    uint64_t sh = part_asset::fnv1a64(source.data(), source.size());
    uint64_t ph = part_asset::fnv1a64(canon_params.data(), canon_params.size());
    // fold (order matters: distinct (source,params) => distinct key)
    return part_asset::fnv1a64(&sh, sizeof sh) ^
           (part_asset::fnv1a64(&ph, sizeof ph) * 1099511628211ull);
}

} // namespace

InstallResult PartGraph::install(const std::vector<ChildRequest>& roots) {
    InstallResult result;
    std::unordered_map<uint64_t, InternalNode> memo;   // memo_key -> node
    std::string error;

    std::vector<std::string> stack;            // module names, for the error path
    std::set<uint64_t> on_stack;               // memo_keys currently being resolved

    // Recursive resolve. Returns memo_key (0 sentinel cannot collide in practice; we
    // also carry a success flag out-of-band via `error`).
    std::function<bool(const ChildRequest&, uint64_t&)> resolve =
        [&](const ChildRequest& req, uint64_t& out_key) -> bool {
            std::string source;
            if (!resolver_.load_source(req.module, source)) {
                error = "missing requires target: " + req.module;
                return false;
            }
            std::string canon = serialize_params(req.params);
            uint64_t key = memo_key_of(source, canon);

            if (on_stack.count(key)) {                 // back-edge => cycle
                std::string path;
                for (const auto& m : stack) path += m + " -> ";
                path += req.module;
                error = "cycle detected: " + path;
                return false;
            }
            auto it = memo.find(key);
            if (it != memo.end()) { out_key = key; return true; }   // memoized (DAG reuse)

            std::vector<ChildRequest> kids;
            if (!resolver_.get_requires(req.module, req.params, kids)) {
                error = "module failed to evaluate requires: " + req.module;
                return false;
            }

            stack.push_back(req.module);
            on_stack.insert(key);

            std::vector<uint64_t> child_keys, child_hashes;
            std::vector<std::string> child_modules, child_params;
            for (const auto& kid : kids) {
                uint64_t ck = 0;
                if (!resolve(kid, ck)) { stack.pop_back(); on_stack.erase(key); return false; }
                child_keys.push_back(ck);
                child_hashes.push_back(memo.at(ck).resolved_hash);
                child_modules.push_back(kid.module);
                child_params.push_back(params_to_json(kid.params));
            }

            stack.pop_back();
            on_stack.erase(key);

            InternalNode node;
            node.memo_key      = key;
            node.module        = req.module;
            node.source        = source;
            node.params        = req.params;
            node.child_keys    = child_keys;
            node.child_hashes  = child_hashes;   // SP-1 sorts internally; ok unsorted here
            node.child_modules = child_modules;  // parallel to child_hashes (name->hash map downstream)
            node.child_params  = child_params;   // parallel canonical params JSON (variant selection key)
            // Hash authority is SP-2 (master C-2): ask the baker, never compute here.
            // The host merges static+override params before folding, so it sees defaults
            // SP-3 cannot. 0 => resolve failure (fail-closed).
            node.resolved_hash = baker_.resolve_hash(source, req.params, child_hashes);
            if (node.resolved_hash == 0) {
                error = "failed to resolve hash for part: " + req.module;
                return false;
            }
            memo.emplace(key, std::move(node));
            out_key = key;
            return true;
        };

    std::vector<uint64_t> root_keys;
    for (const auto& r : roots) {
        uint64_t k = 0;
        if (!resolve(r, k)) { result.error = error; return result; }
        root_keys.push_back(k);
        // Surface the child-folded resolved hash so callers (e.g. LocalProvider's
        // manifest) reference the SAME hash the graph bakes to, not an unfolded recompute.
        result.root_hashes.push_back(memo.at(k).resolved_hash);
    }

    // Topological (post-order) bake over the reachable set from roots: a node is baked
    // only after all its children. DFS post-order on a DAG yields children-first order.
    std::set<uint64_t> baked_or_present;     // memo_keys already handled
    std::vector<uint64_t> topo;              // memo_keys in children-first order
    std::function<void(uint64_t)> post = [&](uint64_t key) {
        if (baked_or_present.count(key)) return;
        baked_or_present.insert(key);
        const InternalNode& n = memo.at(key);
        for (uint64_t ck : n.child_keys) post(ck);
        topo.push_back(key);
    };
    for (uint64_t rk : root_keys) post(rk);

    for (uint64_t key : topo) {
        const InternalNode& n = memo.at(key);
        if (baker_.cached(n.resolved_hash)) {
            ++result.hits;
        } else {
            if (!baker_.bake(n.source, n.params, n.child_hashes, n.child_modules,
                             n.child_params, n.resolved_hash)) {
                result.error = "bake failed for part: " + n.module;
                return result;
            }
            result.baked.push_back(n.resolved_hash);
        }
        if (!baker_.bake_lod_variants(n.source, n.params, n.child_hashes,
                                      n.resolved_hash)) {
            result.error = "lod-variant bake failed for part: " + n.module;
            return result;
        }
    }
    result.ok = true;
    return result;
}

bool PartGraph::read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out,
                              std::vector<bool>* expand_out,
                              std::vector<bool>* tileset_out) {
    std::string path = world_data_dir + "/" + world + "/world.manifest";
    std::ifstream in(path);
    if (!in) {
        error_out = "world manifest not found: " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        // trim leading/trailing whitespace
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;        // blank
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(b, e - b + 1);
        if (trimmed.empty() || trimmed[0] == '#') continue; // comment
        std::istringstream tokens(trimmed);
        std::string name, flag;
        tokens >> name;
        if (name == "light") continue;  // light lines are owned by world_lights::parse_lights
        bool expand = false, tileset = false;
        while (tokens >> flag) {
            if (flag == "expand") expand = true;
            else if (flag == "tileset") tileset = true;
            else {
                error_out = "unknown manifest flag '" + flag + "' for root " + name;
                return false;
            }
        }
        if (expand && tileset) {
            error_out = "root " + name + " cannot be both tileset and expand";
            return false;
        }
        roots_out.push_back(ChildRequest{ name, Params{} });
        if (expand_out)  expand_out->push_back(expand);
        if (tileset_out) tileset_out->push_back(tileset);
    }
    return true;
}

} // namespace part_graph

#if defined(MATTER_HAVE_SCRIPT_HOST)
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace part_graph {

// params_to_json is defined in the always-compiled section above (install() keys
// child placements by it); the host path reuses it for resolve_hash/bake.

// Inverse: a flat JSON object {"k":num|bool|"str", ...} -> Params. SP-3 v1 only sees
// the shapes eval_requires emits (flat numbers/bools/strings), so a tiny hand parser
// suffices; reuse the host's own emitter contract rather than a full JSON lib.
Params params_from_json(const std::string& json);  // defined below

FileModuleResolver::FileModuleResolver(script_host::ScriptHost& host, std::string schemas_dir)
    : host_(host), schemas_dir_(std::move(schemas_dir)) {}

bool FileModuleResolver::load_source(const std::string& module, std::string& out) {
    std::ifstream in(schemas_dir_ + "/" + module + ".js", std::ios::binary);
    if (!in) return false;
    std::ostringstream ss; ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool FileModuleResolver::get_requires(const std::string& module, const Params& params,
                                      std::vector<ChildRequest>& out) {
    std::string source;
    if (!load_source(module, source)) return false;
    // SP-2 eval_requires: eval module top level (no build()), read `static requires`.
    std::vector<script_host::RequiredChild> kids =
        host_.eval_requires(source, params_to_json(params));
    out.clear();
    out.reserve(kids.size());
    for (const auto& k : kids)
        out.push_back(ChildRequest{ k.module_specifier, params_from_json(k.params_json) });
    return true;   // (a thrown `requires` surfaces as a host error -> empty + caller errors)
}

HostBaker::HostBaker(script_host::ScriptHost& host, std::string parts_dir)
    : host_(host), parts_dir_(std::move(parts_dir)) {}

uint64_t HostBaker::resolve_hash(const std::string& source, const Params& params,
                                 const std::vector<uint64_t>& child_hashes) {
    return host_.resolve_hash(source, params_to_json(params),
                              child_hashes.data(), child_hashes.size());
}

bool HostBaker::cached(uint64_t resolved_hash) {
    std::string path = parts_dir_ + "/" + part_asset::cache_path_resolved(resolved_hash);
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return false;
    // Validate the header: magic must be 'TRAP', version 2, and the embedded
    // resolved_hash must match `resolved_hash`. Otherwise the file is stale
    // (a previous bake with the SAME filename hash-key but DIFFERENT resolved
    // hash — happens when a schema is edited between runs and the old .part
    // was left behind).  Treat mismatch as cache miss so we re-bake.
    // Header layout (see part_asset_v2.cpp:write_file_atomic):
    //   [0..4)  magic uint32   ('TRAP' little-endian = 0x50415254)
    //   [4..8)  version uint32 (currently 2)
    //   [8..16) resolved_hash XOR version (uint64, obfuscation)
    struct { uint32_t magic; uint32_t version; uint64_t hash_xor; } hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!in.good()) return false;
    if (hdr.magic != 0x50415254u || hdr.version != 2u) return false;
    return hdr.hash_xor == (resolved_hash ^ (uint64_t)hdr.version);
}

bool HostBaker::bake(const std::string& source, const Params& params,
                     const std::vector<uint64_t>& child_hashes,
                     const std::vector<std::string>& child_modules,
                     const std::vector<std::string>& child_params, uint64_t resolved_hash) {
    // SP-2 bake_source recomputes the same hash and writes the .part via save_v2.
    // Pass parts_dir_ so bake_source writes to an absolute path rather than a
    // cwd-relative "parts/<hash>.part" (Task 3 Phase B: cwd-independence).
    script_host::BakeOptions bopts;
    bopts.parts_dir = parts_dir_;
    script_host::BakeResult r = host_.bake_source(
        source, params_to_json(params), bopts,
        child_hashes.data(), child_hashes.size(),
        child_modules.data(), child_params.data());
    // The hash SP-3 memoized must equal where the .part landed (master C-2 guarantee).
    if (!r.error.ok) {
        std::fprintf(stderr, "  HostBaker::bake: %s\n", r.error.message.c_str());
        return false;
    }
    if (r.resolved_hash != resolved_hash) {
        std::fprintf(stderr,
            "  HostBaker::bake: resolved_hash mismatch: expected %016llx got %016llx\n",
            (unsigned long long)resolved_hash, (unsigned long long)r.resolved_hash);
        return false;
    }
    return true;
}

bool HostBaker::bake_lod_variants(const std::string& source, const Params& params,
                                  const std::vector<uint64_t>& child_hashes,
                                  uint64_t resolved_hash) {
    script_host::ScriptHost::LodBudgetSpec spec = host_.eval_lod_budgets(source);
    if (spec.budgets.empty()) return true;                    // not opted in
    if (!child_hashes.empty()) {
        printf("HostBaker: lodBudgets on a part with children is unsupported; skipping\n");
        return true;
    }
    const std::string sidecar = parts_dir_ + "/" + part_asset::cache_path_lods(resolved_hash);
    {
        // content-addressed fast path: sidecar exists AND every referenced .part exists.
        // If a variant was pruned from the cache, fall through and re-bake the whole ladder.
        part_asset::LodVariants existing;
        if (part_asset::load_lod_sidecar(sidecar, existing)) {
            bool all_present = true;
            for (uint64_t h : existing.hashes) {
                const std::string vpath = parts_dir_ + "/" + part_asset::cache_path_resolved(h);
                std::ifstream probe(vpath);
                if (!probe.good()) { all_present = false; break; }
            }
            if (all_present) return true;
        }
    }

    std::vector<uint64_t> variant_hashes;
    for (double b : spec.budgets) {
        if (b >= 1.0) {
            // Full budget == the main bake: lodBudget defaults to 1.0 in the
            // schema's static params, so the merged params (and hash) match.
            variant_hashes.push_back(resolved_hash);
            continue;
        }
        Params p2 = params;
        p2["lodBudget"] = ParamValue::number(b);
        script_host::BakeOptions vopts;
        vopts.parts_dir = parts_dir_;
        script_host::BakeResult r = host_.bake_source(source, params_to_json(p2), vopts);
        if (!r.error.ok) return false;
        variant_hashes.push_back(r.resolved_hash);
    }

    const std::string tmp = sidecar + ".tmp";
    {
        std::ofstream o(tmp);
        o << spec.anchor_size << "\n";
        for (size_t i = 0; i < spec.budgets.size(); ++i) {
            char hex[17];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)variant_hashes[i]);
            o << spec.budgets[i] << " " << hex << "\n";
        }
        if (!o.good()) return false;
    }
    return std::rename(tmp.c_str(), sidecar.c_str()) == 0;
}

// Minimal flat-object JSON parser for the shapes eval_requires emits (flat
// number|bool|"string"; SP-3 v1 strings carry no escapes). Unknown shapes -> skip.
Params params_from_json(const std::string& json) {
    Params out;
    size_t i = 0, n = json.size();
    auto skip_ws = [&]{ while (i < n && (json[i]==' '||json[i]=='\t'||json[i]=='\n'||json[i]=='\r')) ++i; };
    auto parse_str = [&](std::string& s) -> bool {
        if (i >= n || json[i] != '"') return false;
        ++i; size_t start = i;
        while (i < n && json[i] != '"') ++i;
        if (i >= n) return false;
        s = json.substr(start, i - start); ++i; return true;
    };
    skip_ws();
    if (i >= n || json[i] != '{') return out; ++i;
    skip_ws();
    if (i < n && json[i] == '}') return out;
    while (i < n) {
        skip_ws();
        std::string key;
        if (!parse_str(key)) break;
        skip_ws();
        if (i >= n || json[i] != ':') break; ++i;
        skip_ws();
        if (i < n && json[i] == '"') {
            std::string v; if (!parse_str(v)) break;
            out[key] = ParamValue::string_(v);
        } else if (json.compare(i, 4, "true") == 0) {
            out[key] = ParamValue::boolean_(true); i += 4;
        } else if (json.compare(i, 5, "false") == 0) {
            out[key] = ParamValue::boolean_(false); i += 5;
        } else {
            size_t start = i;
            while (i < n && json[i] != ',' && json[i] != '}') ++i;
            out[key] = ParamValue::number(std::strtod(json.c_str() + start, nullptr));
        }
        skip_ws();
        if (i < n && json[i] == ',') { ++i; continue; }
        if (i < n && json[i] == '}') break;
    }
    return out;
}

} // namespace part_graph
#endif // MATTER_HAVE_SCRIPT_HOST
