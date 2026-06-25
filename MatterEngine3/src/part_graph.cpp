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
            for (const auto& kid : kids) {
                uint64_t ck = 0;
                if (!resolve(kid, ck)) { stack.pop_back(); on_stack.erase(key); return false; }
                child_keys.push_back(ck);
                child_hashes.push_back(memo.at(ck).resolved_hash);
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
        if (baker_.cached(n.resolved_hash)) { ++result.hits; continue; }
        if (!baker_.bake(n.source, n.params, n.child_hashes, n.resolved_hash)) {
            result.error = "bake failed for part: " + n.module;
            return result;
        }
        result.baked.push_back(n.resolved_hash);
    }
    result.ok = true;
    return result;
}

bool PartGraph::read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out) {
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
        std::string name = line.substr(b, e - b + 1);
        if (name.empty() || name[0] == '#') continue; // comment
        roots_out.push_back(ChildRequest{ name, Params{} });
    }
    return true;
}

} // namespace part_graph
