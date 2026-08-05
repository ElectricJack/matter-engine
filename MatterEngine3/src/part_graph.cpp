#include "part_graph.h"
#include "matter/lod_contract.h"  // W5: kMaxSerializedLodLevels (exclude-mask bit width guard)
#include "part_asset_v2.h"   // SP-1 (MatterEngine3, via -I../include): compute_resolved_hash,
                            //   cache_path_resolved; pulls in v1 part_asset.h for fnv1a64
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <new>          // std::bad_alloc
#include <regex>        // shared-lib import scan (Task 9)
#include <set>
#include <sstream>
#include <stdexcept>    // std::exception
#include <string>
#include <unordered_map>
#include <vector>
#include "animation/anim_bundle.h"

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

InstallResult PartGraph::install(const std::vector<ChildRequest>& roots,
                                  part_graph_snapshot::Snapshot* snap,
                                  BakePolicy policy) {
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

    // Resolve all roots. Two-tier policy:
    //   - Hard errors (abort whole install): cycle detection, missing required module
    //     (structural graph problems no sibling can work around).
    //   - Soft errors (skip-and-continue): hash-resolution failure, script eval failure
    //     (a broken/invalid single root — siblings continue).
    // Cycle and missing-module errors embed "cycle" or "missing requires target" in the
    // error string; everything else is treated as a soft per-root failure.
    result.root_hashes.resize(roots.size(), 0);
    std::vector<std::pair<uint64_t, size_t>> root_keys_with_idx;  // (memo_key, root_index)
    for (size_t ri = 0; ri < roots.size(); ++ri) {
        const auto& r = roots[ri];
        uint64_t k = 0;
        if (!resolve(r, k)) {
            // Hard-error if the failure indicates a structural graph problem.
            if (error.find("cycle") != std::string::npos ||
                error.find("missing requires target") != std::string::npos) {
                result.error = error;
                return result;
            }
            // Soft error: record as a failed root and continue sibling roots.
            FailedPart fp;
            fp.module        = r.module;
            fp.resolved_hash = 0;
            fp.error         = error;
            result.failed.push_back(std::move(fp));
            // result.root_hashes[ri] already 0 from resize.
            error.clear();
            continue;
        }
        root_keys_with_idx.push_back({k, ri});
        // Surface the child-folded resolved hash so callers (e.g. LocalProvider's
        // manifest) reference the SAME hash the graph bakes to, not an unfolded recompute.
        result.root_hashes[ri] = memo.at(k).resolved_hash;
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
    for (const auto& kp : root_keys_with_idx) post(kp.first);

    // Task 13 (Phase C): build bake_plan from memo (all resolved nodes).
    // Done here — after resolve is complete and topo is known — so it covers
    // every node regardless of BakePolicy.  The snapshot is also populated here
    // (it only reads memo; moving it earlier than the bake loop is safe per the
    // verified precondition that resolve fully populates memo before the bake loop).
    for (const auto& kv : memo) {
        const InternalNode& n = kv.second;
        BakeInputs bi;
        bi.module        = n.module;
        bi.source        = n.source;
        bi.params        = n.params;
        bi.child_hashes  = n.child_hashes;
        bi.child_modules = n.child_modules;
        bi.child_params  = n.child_params;
        result.bake_plan.emplace(n.resolved_hash, std::move(bi));
    }

    // Task 9: populate the snapshot (if requested) from the completed resolve.
    // Moved from below the bake loop: snapshot only reads memo, so it is safe
    // to fill it here (before the bake loop) for all BakePolicy variants.
    if (snap) {
        // Build a set of root module names for is_root tagging.
        std::set<std::string> root_modules;
        for (const auto& kp : root_keys_with_idx)
            root_modules.insert(memo.at(kp.first).module);

        // Regex for shared-lib import detection:
        // matches: from ['"]shared-lib/(<name>)['"]
        // or:      import ['"]shared-lib/(<name>)['"]
        static const std::regex kSharedImportRe(
            R"((?:from|import)\s+['"]shared-lib/([^'"]+)['"])");

        for (const auto& kv : memo) {
            const InternalNode& n = kv.second;

            // Build snapshot node (one per unique module name; if the same
            // module appears with different params it may appear multiple times
            // in memo — take the first seen for the snapshot, matching the
            // module-identity contract from the brief).
            if (snap->nodes.count(n.module)) continue;

            part_graph_snapshot::Node snode;
            snode.module        = n.module;
            snode.params_json   = params_to_json(n.params);
            snode.resolved_hash = n.resolved_hash;
            snode.is_root       = root_modules.count(n.module) > 0;

            // Children: use child_modules (parallel to child_hashes).
            // Deduplicate while preserving insertion order.
            {
                std::set<std::string> seen_children;
                for (const auto& cm : n.child_modules) {
                    if (seen_children.insert(cm).second)
                        snode.children.push_back(cm);
                }
            }

            // shared_imports: scan source for `from/import 'shared-lib/<X>'`.
            {
                std::set<std::string> seen_imports;
                auto begin = std::sregex_iterator(n.source.begin(), n.source.end(),
                                                  kSharedImportRe);
                auto end   = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    std::string imp = (*it)[1].str();
                    if (seen_imports.insert(imp).second)
                        snode.shared_imports.push_back(imp);
                }
            }

            // Resolve the complete import closure so snapshots record the exact
            // project/engine file selected for direct and transitive imports.
            for (const auto& selected : resolver_.shared_sources_for(n.source)) {
                std::string imp = selected.first;
                const std::string prefix = "shared-lib/";
                if (imp.rfind(prefix, 0) == 0) imp.erase(0, prefix.size());
                if (imp.size() >= 3 &&
                    imp.compare(imp.size() - 3, 3, ".js") == 0)
                    imp.resize(imp.size() - 3);
                if (std::find(snode.shared_imports.begin(),
                              snode.shared_imports.end(), imp) ==
                    snode.shared_imports.end())
                    snode.shared_imports.push_back(imp);
                if (std::find(snode.shared_source_paths.begin(),
                              snode.shared_source_paths.end(), selected.second) ==
                    snode.shared_source_paths.end())
                    snode.shared_source_paths.push_back(selected.second);
            }

            // source_path: ask the resolver (FileModuleResolver overrides to return
            // <schemas_dir>/<module>.js; base class returns "").
            snode.source_path = resolver_.source_path_for(n.module);

            snap->nodes.emplace(n.module, std::move(snode));
        }

        // Build by_file and by_import indices.
        for (auto& kv2 : snap->nodes) {
            part_graph_snapshot::Node& sn = kv2.second;
            if (!sn.source_path.empty()) {
                auto& bfv = snap->by_file[sn.source_path];
                if (std::find(bfv.begin(), bfv.end(), sn.module) == bfv.end())
                    bfv.push_back(sn.module);
            }
            for (const auto& shared_path : sn.shared_source_paths) {
                auto& bfv = snap->by_file[shared_path];
                if (std::find(bfv.begin(), bfv.end(), sn.module) == bfv.end())
                    bfv.push_back(sn.module);
            }
            for (const auto& imp : sn.shared_imports) {
                auto& biv = snap->by_import[imp];
                if (std::find(biv.begin(), biv.end(), sn.module) == biv.end())
                    biv.push_back(sn.module);
            }
        }
    }

    // Build root resolved_hash set for BakePolicy::RootsOnly filtering.
    std::set<uint64_t> root_resolved_hashes;
    for (const auto& kp : root_keys_with_idx)
        root_resolved_hashes.insert(memo.at(kp.first).resolved_hash);

    // Task 7: skip-and-continue policy.
    // failed_keys: memo_keys of nodes that failed (bake or lod-variants failure).
    // A node whose ANY child is in failed_keys is itself skipped and recorded as
    // "missing child" in result.failed[]. Siblings are unaffected.
    std::set<uint64_t> failed_keys;

    for (uint64_t key : topo) {
        const InternalNode& n = memo.at(key);

        // Task 13 (Phase C): RootsOnly policy — skip baking non-root nodes.
        // They are already captured in bake_plan for on-demand bake later.
        // bake_lod_variants for skipped nodes is deferred to ensure_part_baked.
        if (policy == BakePolicy::RootsOnly &&
            root_resolved_hashes.find(n.resolved_hash) == root_resolved_hashes.end()) {
            // Intentional: skipped (non-root) nodes are never added to failed_keys,
            // so the root's child-failure guard below will always pass under RootsOnly.
            // Child-bake failures are deferred to ensure_part_baked at publish time.
            continue;
        }

        // Check if any direct child failed -> skip this node.
        bool child_failed = false;
        std::string missing_child_name;
        for (size_t ci = 0; ci < n.child_keys.size(); ++ci) {
            if (failed_keys.count(n.child_keys[ci])) {
                child_failed = true;
                // Use child_modules[ci] for the error message (parallel to child_keys).
                missing_child_name = (ci < n.child_modules.size()) ? n.child_modules[ci] : "unknown";
                break;
            }
        }
        if (child_failed) {
            failed_keys.insert(key);
            FailedPart fp;
            fp.module        = n.module;
            fp.resolved_hash = n.resolved_hash;
            fp.error         = "missing child: " + missing_child_name;
            result.failed.push_back(std::move(fp));
            continue;
        }

        if (baker_.cached(n.resolved_hash)) {
            ++result.hits;
        } else {
            // Task 7: exceptions from baker_.bake() (e.g. test_fault_hook throws) are
            // caught here and treated identically to a false return value (skip-and-continue).
            bool bake_ok = false;
            std::string bake_err;
            try {
                // Task 2: notify baker of the module being baked (for transient routing)
                baker_.set_baking_module(n.module);
                bake_ok = baker_.bake(n.source, n.params, n.child_hashes, n.child_modules,
                                      n.child_params, n.resolved_hash);
            } catch (std::bad_alloc&) {
                bake_err = "out of memory (bad_alloc) baking part: " + n.module;
            } catch (std::exception& e) {
                bake_err = std::string("exception baking part: ") + n.module + ": " + e.what();
            } catch (...) {
                bake_err = "unknown exception baking part: " + n.module;
            }
            if (!bake_ok) {
                // Task 7 skip-and-continue: record the failure, mark this key failed,
                // continue to sibling nodes. Dependents will be skipped below.
                failed_keys.insert(key);
                FailedPart fp;
                fp.module        = n.module;
                fp.resolved_hash = n.resolved_hash;
                fp.error         = bake_err.empty() ? ("bake failed for part: " + n.module)
                                                    : bake_err;
                result.failed.push_back(std::move(fp));
                continue;
            }
            result.baked.push_back(n.resolved_hash);
        }
        // W5 (Part Workbench, static lods): safe on both the fresh-bake and
        // cache-hit paths above (see bake_static_lods's own doc comment for
        // the fast/slow recovery split); run before bake_lod_variants so its
        // fast path can see the state a fresh bake() just left on the host.
        // Soft-fail (log via HostBaker, do not abort the graph install) unlike
        // bake_lod_variants below: an authoring nicety failing should not take
        // down an otherwise-successful part bake.
        if (!baker_.bake_static_lods(n.source, n.params, n.child_hashes,
                                     n.child_modules, n.child_params, n.resolved_hash)) {
            std::fprintf(stderr,
                "  PartGraph::install: static-lods bake failed for part %s (non-fatal, "
                "authored ladder unavailable this run)\n", n.module.c_str());
        }
        if (!baker_.bake_lod_variants(n.source, n.params, n.child_hashes,
                                      n.resolved_hash)) {
            // lod-variant failure remains a hard error (sidecar is non-optional for
            // parts that opt in; a partial ladder is worse than a missing one).
            result.error = "lod-variant bake failed for part: " + n.module;
            return result;
        }
    }

    // ok=true even with partial failures; caller checks result.failed for details.
    result.ok = true;

    // Propagate bake-phase failures: for roots whose node failed during bake,
    // zero out root_hashes[original_root_index] so callers skip placing them.
    for (const auto& kp : root_keys_with_idx) {
        uint64_t memo_key = kp.first;
        size_t   orig_idx = kp.second;
        if (failed_keys.count(memo_key)) {
            result.root_hashes[orig_idx] = 0;
        }
    }

    return result;
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

std::vector<std::pair<std::string, std::string>>
FileModuleResolver::shared_sources_for(const std::string& source) {
    module_resolver::FoldResult fold;
    std::string err;
    if (!host_.fold_sources_cached(source, fold, err)) return {};
    std::vector<std::pair<std::string, std::string>> selected;
    selected.reserve(fold.modules.size());
    for (const auto& module : fold.modules)
        selected.emplace_back(module.specifier, module.source_path);
    return selected;
}

HostBaker::HostBaker(script_host::ScriptHost& host, std::string parts_dir)
    : host_(host), parts_dir_(std::move(parts_dir)) {}

uint64_t HostBaker::resolve_hash(const std::string& source, const Params& params,
                                 const std::vector<uint64_t>& child_hashes) {
    return host_.resolve_hash(source, params_to_json(params),
                              child_hashes.data(), child_hashes.size());
}

bool HostBaker::cached(uint64_t resolved_hash) {
    // A scratch PART selects scratch as the artifact root for this hash.  This
    // intentionally mirrors PartStore's scratch-first root choice: a linked
    // scratch PART with a torn MANM/MACM sibling set must be rebuilt in scratch,
    // never silently accepted from a persistent-cache generation.
    auto check_path = [this, resolved_hash](const std::string& base_dir) -> bool {
        if (base_dir.empty()) return false;
        std::string path = base_dir + "/" + part_asset::cache_path_resolved(resolved_hash);
        if (!part_asset::is_cache_artifact_header_compatible(path, resolved_hash, part_asset::kFormatVersionV2)) return false;
        std::optional<part_asset::PartAnimationLink> link;
        if (!part_asset::load_animation_link(path, resolved_hash, link)) return false;
        if (!link) {
            uint64_t static_fingerprint = 0;
            if (!part_asset::load_static_part_snapshot(path, resolved_hash,
                                                       static_fingerprint)) return false;
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
            if (cache_validation_hook_for_tests_) cache_validation_hook_for_tests_();
#endif
            uint64_t final_static_fingerprint = 0;
            return part_asset::load_static_part_snapshot(path, resolved_hash,
                                                         final_static_fingerprint) &&
                   final_static_fingerprint == static_fingerprint;
        }
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
        if (cache_validation_hook_for_tests_) cache_validation_hook_for_tests_();
#endif
        BLASManager checked; matter::animation::AnimAsset asset; matter::animation::Diagnostics diagnostics;
        if (!matter::animation::load_committed_animation_bundle(base_dir, resolved_hash, checked, asset, diagnostics)) return false;

        // The PART and its sibling manifest are independently published.  A
        // complete bundle that changed after the initial ANLK read is not the
        // generation this probe selected, so reject it and let the graph
        // rebake/retry from a fresh root snapshot.  The runtime loader makes
        // the identical stable-link check before accepting a loaded part.
        std::optional<part_asset::PartAnimationLink> final_link;
        if (!part_asset::load_animation_link(path, resolved_hash, final_link) || !final_link ||
            final_link->nonce_high != link->nonce_high ||
            final_link->nonce_low != link->nonce_low ||
            asset.nonce.high != link->nonce_high || asset.nonce.low != link->nonce_low) return false;
        return true;
    };

    if (!transient_dir_.empty()) {
        std::error_code ec;
        const std::string scratch_part =
            transient_dir_ + "/" + part_asset::cache_path_resolved(resolved_hash);
        if (std::filesystem::exists(scratch_part, ec) && !ec)
            return check_path(transient_dir_);
    }
    return check_path(parts_dir_);
}

bool HostBaker::bake(const std::string& source, const Params& params,
                     const std::vector<uint64_t>& child_hashes,
                     const std::vector<std::string>& child_modules,
                     const std::vector<std::string>& child_params, uint64_t resolved_hash) {
    // SP-2 bake_source recomputes the same hash and writes the .part via save_v2.
    // Pass parts_dir_ so bake_source writes to an absolute path rather than a
    // cwd-relative "parts/<hash>.bundle" (Task 3 Phase B: cwd-independence).
    // Task 2: route transient modules to scratch_dir instead.
    script_host::BakeOptions bopts;
    bopts.parts_dir = is_transient(current_module_) ? transient_dir_ : parts_dir_;
    bopts.world = world_;  // Task 5: thread world binding for terrainVolume
    bopts.observer = observer_;  // W3: thread the optional per-rung observer
    script_host::BakeResult r = host_.bake_source(
        source, params_to_json(params), bopts,
        child_hashes.data(), child_hashes.size(),
        child_modules.data(), child_params.data());
    // W5: invalidate the fast-path cache up-front so a failed bake below never
    // leaves a stale prior part's data readable as if it were current.
    last_baked_hash_ = 0;
    last_child_modules_placed_.clear();
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
    last_child_modules_placed_ = r.child_modules_placed;
    last_baked_hash_ = resolved_hash;
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
    // Task 2: route sidecar to scratch if transient
    const std::string base_dir = is_transient(current_module_) ? transient_dir_ : parts_dir_;
    const std::string sidecar = base_dir + "/" + part_asset::cache_path_lods(resolved_hash);
    {
        // content-addressed fast path: sidecar exists AND every referenced .part exists.
        // If a variant was pruned from the cache, fall through and re-bake the whole ladder.
        part_asset::LodVariants existing;
        if (part_asset::load_lod_sidecar(sidecar, resolved_hash, existing)) {
            bool all_present = true;
            for (uint64_t h : existing.hashes) {
                const std::string vpath = base_dir + "/" + part_asset::cache_path_resolved(h);
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
        vopts.parts_dir = base_dir;
        script_host::BakeResult r = host_.bake_source(source, params_to_json(p2), vopts);
        if (!r.error.ok) return false;
        variant_hashes.push_back(r.resolved_hash);
    }

    // M4: the sidecar is a bundle section now, so its serializer moved next
    // to its reader in part_asset_v2 instead of living inline here. (It also
    // gains an atomic publish: this used to be std::rename, which on Windows
    // fails when the target exists -- the same defect the resolve cache had.)
    part_asset::LodVariants variants;
    variants.anchor_size = spec.anchor_size;
    variants.budgets     = spec.budgets;
    variants.hashes      = variant_hashes;
    return part_asset::save_lod_sidecar(sidecar, resolved_hash, variants);
}

bool HostBaker::bake_static_lods(const std::string& source, const Params& params,
                                 const std::vector<uint64_t>& child_hashes,
                                 const std::vector<std::string>& child_modules,
                                 const std::vector<std::string>& child_params,
                                 uint64_t resolved_hash) {
    script_host::ScriptHost::LodAuthoring lods = host_.eval_lods(source);
    if (lods.empty()) return true;  // not opted in
    if (lods.size() > matter::kMaxSerializedLodLevels) {
        std::fprintf(stderr,
            "HostBaker: static lods (%zu levels) exceeds the %zu-level serialized "
            "cap; skipping per-LOD authoring for this part\n",
            lods.size(), matter::kMaxSerializedLodLevels);
        return true;  // fail-closed: treat as not opted in, do not fail the whole bake
    }

    const std::string base_dir = is_transient(current_module_) ? transient_dir_ : parts_dir_;
    const std::string base_params_json = params_to_json(params);

    // M3: the authored switch distances and named generators. Computed here,
    // BEFORE the fast path, so the fast path can insist a cached sidecar
    // already carries them — a sidecar written by a pre-M3 binary at this same
    // resolved hash would otherwise be accepted as current while silently
    // missing the whole authored ladder.
    std::vector<double>      want_at(lods.size(), -1.0);
    std::vector<std::string> want_gen(lods.size());
    for (size_t i = 0; i < lods.size(); ++i) {
        if (lods[i].has_at) want_at[i] = lods[i].at;
        if (!lods[i].gen.empty())
            want_gen[i] = lods[i].gen + " " + lods[i].gen_params_json;
        // §3.4's terminal impostor rides the SAME level_gen slot as a named
        // generator. It is not one -- it takes over from the coarsest mesh
        // rung rather than deriving from LOD0 -- but reusing the slot means it
        // round-trips through the plan format unchanged, it counts toward
        // drives_ladder() (so `LOD.impostor({})` with no other M3 field is not
        // silently dropped), and the fast path's `existing.level_gen ==
        // want_gen` staleness check covers it for free. eval_lods has already
        // guaranteed the entry carries no real generator.
        if (lods[i].impostor) want_gen[i] = "impostor {}";
    }

    // Content-addressed fast path (mirrors bake_lod_variants): a fresh sidecar
    // with every referenced .part still present means the plan is current.
    const std::string sidecar =
        base_dir + "/" + part_asset::cache_path_static_lods(resolved_hash);
    {
        part_asset::StaticLodPlan existing;
        if (part_asset::load_static_lod_plan(sidecar, resolved_hash, existing) &&
            existing.level_hashes.size() == lods.size() &&
            existing.level_at == want_at && existing.level_gen == want_gen) {
            bool all_present = true;
            for (uint64_t h : existing.level_hashes) {
                const std::string vpath = base_dir + "/" + part_asset::cache_path_resolved(h);
                std::ifstream probe(vpath);
                if (!probe.good()) { all_present = false; break; }
            }
            if (all_present) return true;
        }
    }

    // Recover the base bake's child-table module order + merged params.
    // FAST path (zero extra cost, the common case): reuse what the
    // immediately-preceding bake() call for this SAME resolved_hash already
    // left cached on this HostBaker. SLOW/robust path (a cache hit whose
    // sidecar is missing/stale — bake() was never called this process for
    // this part, e.g. `static lods` was just added to an already-cached
    // part): one content-addressed, idempotent bake_source call reproduces
    // the exact bytes already on disk (same hash), safe but not free.
    std::vector<std::string> child_modules_placed;
    std::string base_merged;
    if (last_baked_hash_ == resolved_hash) {
        child_modules_placed = last_child_modules_placed_;
        base_merged = host_.last_merged_params();
    } else {
        script_host::BakeOptions base_opts;
        base_opts.parts_dir = base_dir;
        base_opts.world = world_;
        script_host::BakeResult base_r = host_.bake_source(
            source, base_params_json, base_opts,
            child_hashes.data(), child_hashes.size(),
            child_modules.data(), child_params.data());
        if (!base_r.error.ok || base_r.resolved_hash != resolved_hash) return false;
        child_modules_placed = base_r.child_modules_placed;
        base_merged = host_.last_merged_params();
    }

    const bool has_children = !child_hashes.empty();
    if (has_children) {
        bool any_params = false;
        for (const auto& L : lods) if (L.has_params) { any_params = true; break; }
        if (any_params)
            std::printf(
                "HostBaker: static lods `params` levels on a part with children are "
                "unsupported (mirrors bake_lod_variants's own children restriction); "
                "those levels fall back to decimation from LOD0 -- `exclude` still applies\n");
    }

    part_asset::StaticLodPlan plan;
    plan.level_hashes.resize(lods.size());
    plan.level_exclude_masks.assign(lods.size(), 0u);
    // M3: the authored switch distance and the named generator ride through to
    // the flatten stage, which is the only place that can turn metres into the
    // normalized threshold the runtime table stores (it needs the baked
    // cluster radius; see part_flatten's static-lod ladder). Nothing is
    // generated here.
    plan.level_at  = want_at;
    plan.level_gen = want_gen;

    for (size_t i = 0; i < lods.size(); ++i) {
        const auto& L = lods[i];

        // exclude mask: bit k = child at child-table position k is dropped at
        // this level, matched by module name against the base bake's placement
        // order. Silently ignores exclude names that name no placed child
        // (authoring typo) rather than failing the bake -- the source is the
        // authority the user can inspect/fix, not a hard gate here.
        uint32_t mask = 0;
        if (!L.exclude.empty()) {
            for (size_t k = 0; k < child_modules_placed.size() && k < 32; ++k)
                for (const auto& ex : L.exclude)
                    if (child_modules_placed[k] == ex) { mask |= (1u << k); break; }
        }
        plan.level_exclude_masks[i] = mask;

        if (!L.has_params || has_children) {
            plan.level_hashes[i] = resolved_hash;  // decimate from LOD0 (today's ladder)
            continue;
        }

        // Fresh build for this level: base-then-level params merge (shallow;
        // associative with bake_source's own internal Object.assign, see
        // merge_json_shallow's doc comment), seeded from the BASE merged
        // params -- THE SEEDING RULE -- so this level draws the same rng
        // stream as LOD0 and stays structurally the same tree.
        const std::string level_params =
            host_.merge_json_shallow(base_params_json, L.params_json);
        script_host::BakeOptions lvl_opts;
        lvl_opts.parts_dir = base_dir;
        lvl_opts.world = world_;
        lvl_opts.seed_params_json = base_merged;
        script_host::BakeResult lvl_r = host_.bake_source(source, level_params, lvl_opts);
        if (!lvl_r.error.ok) {
            std::fprintf(stderr, "  HostBaker::bake_static_lods: level %zu build failed: %s\n",
                        i, lvl_r.error.message.c_str());
            return false;
        }
        plan.level_hashes[i] = lvl_r.resolved_hash;
    }

    // M4: the LMSK trailer is GONE. It transposed plan.level_exclude_masks
    // into a child-major bitmask and appended it to the compositional body --
    // and nothing in the engine ever read it back. The only reader was a
    // load_v2 overload with no production caller, so the exclude information
    // made a round trip through the part body purely to be skipped again on
    // load. The plan section below already carries level_exclude_masks, which
    // is where the flatten path actually reads them from. M0 deferred this to
    // M4 because removing a trailer is a format change; M4 invalidates every
    // cache anyway, so here it costs nothing.

    return part_asset::save_static_lod_plan(sidecar, resolved_hash, plan);
}

} // namespace part_graph
#endif // MATTER_HAVE_SCRIPT_HOST
