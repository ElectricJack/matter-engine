#include "shader_source.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::string read_shader(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

static size_t count_occurrences(const std::string& text,
                                const std::string& needle) {
    size_t count = 0;
    for (size_t offset = 0;
         (offset = text.find(needle, offset)) != std::string::npos;
         offset += needle.size())
        ++count;
    return count;
}

static size_t matching_closing_brace(const std::string& text,
                                     size_t opening_brace) {
    if (opening_brace == std::string::npos ||
        opening_brace >= text.size() || text[opening_brace] != '{')
        return std::string::npos;
    int depth = 0;
    for (size_t offset = opening_brace; offset < text.size(); ++offset) {
        if (text[offset] == '{')
            ++depth;
        else if (text[offset] == '}' && --depth == 0)
            return offset;
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// Semantic source contracts.
//
// The predicates below parse just enough GLSL/Make structure to state a
// contract independently of formatting and statement factoring: a guard may be
// one disjunction or several early returns, an RT entry may be a thin wrapper
// around a shared implementation, and so on. Each one is run on the real
// source (must hold) and on mutated in-memory copies that break the contract
// (must fail), so the negative coverage shows that every predicate can fail.
// ---------------------------------------------------------------------------

static size_t matching_closing_paren(const std::string& text,
                                     size_t opening_paren) {
    if (opening_paren == std::string::npos ||
        opening_paren >= text.size() || text[opening_paren] != '(')
        return std::string::npos;
    int depth = 0;
    for (size_t offset = opening_paren; offset < text.size(); ++offset) {
        if (text[offset] == '(')
            ++depth;
        else if (text[offset] == ')' && --depth == 0)
            return offset;
    }
    return std::string::npos;
}

static std::string without_whitespace(const std::string& text) {
    std::string compact;
    for (unsigned char ch : text)
        if (!std::isspace(ch)) compact += static_cast<char>(ch);
    return compact;
}

// Replaces // and /* */ comments with a space so that commented-out code can
// never satisfy a contract. GLSL has no string literals to protect.
static std::string without_comments(const std::string& text) {
    std::string code;
    for (size_t offset = 0; offset < text.size(); ++offset) {
        if (text.compare(offset, 2, "//") == 0) {
            offset = text.find('\n', offset);
            if (offset == std::string::npos) break;
            code += '\n';
        } else if (text.compare(offset, 2, "/*") == 0) {
            offset = text.find("*/", offset + 2);
            if (offset == std::string::npos) break;
            ++offset;
            code += ' ';
        } else {
            code += text[offset];
        }
    }
    return code;
}

// The braces and body of the first function defined by `signature`, or empty.
static std::string function_body(const std::string& text,
                                 const std::string& signature) {
    const size_t start = text.find(signature);
    if (start == std::string::npos) return {};
    const size_t open = text.find('{', start);
    const size_t close = matching_closing_brace(text, open);
    if (close == std::string::npos) return {};
    return text.substr(open, close - open + 1u);
}

// Splits on `separator` wherever it sits outside every bracket pair.
static std::vector<std::string> split_top_level(const std::string& text,
                                                const std::string& separator) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t begin = 0;
    for (size_t offset = 0; offset < text.size(); ++offset) {
        const char ch = text[offset];
        if (ch == '(' || ch == '[' || ch == '{') {
            ++depth;
        } else if (ch == ')' || ch == ']' || ch == '}') {
            --depth;
        } else if (depth == 0 &&
                   text.compare(offset, separator.size(), separator) == 0) {
            parts.push_back(text.substr(begin, offset - begin));
            offset += separator.size() - 1u;
            begin = offset + 1u;
        }
    }
    parts.push_back(text.substr(begin));
    return parts;
}

static std::string without_enclosing_parens(std::string expression) {
    while (expression.size() >= 2u && expression.front() == '(' &&
           matching_closing_paren(expression, 0) == expression.size() - 1u)
        expression = expression.substr(1, expression.size() - 2u);
    return expression;
}

// Whitespace-free arguments of the call whose callee name starts at `call`.
static std::vector<std::string> call_arguments(const std::string& text,
                                               size_t call) {
    const size_t open = text.find('(', call);
    const size_t close = matching_closing_paren(text, open);
    if (close == std::string::npos) return {};
    std::vector<std::string> arguments;
    for (const std::string& argument :
         split_top_level(text.substr(open + 1u, close - open - 1u), ","))
        arguments.push_back(without_whitespace(argument));
    return arguments;
}

// Offsets of every call (not the definition) of `name` in comment-free code.
static std::vector<size_t> call_sites(const std::string& code,
                                      const std::string& name) {
    std::vector<size_t> calls;
    for (size_t at = code.find(name + "("); at != std::string::npos;
         at = code.find(name + "(", at + 1u)) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(code[at - 1])) ||
                       code[at - 1] == '_'))
            continue;
        size_t before = at;
        while (before > 0 &&
               std::isspace(static_cast<unsigned char>(code[before - 1])))
            --before;
        const size_t word =
            before == 0 ? std::string::npos
                        : code.find_last_of(" \t\r\n;{}()=,", before - 1u);
        const std::string previous = code.substr(
            word == std::string::npos ? 0 : word + 1u,
            before - (word == std::string::npos ? 0 : word + 1u));
        // `vec3 name(` / `void name(` is a definition, not a call.
        if (!previous.empty() &&
            (std::isalpha(static_cast<unsigned char>(previous[0])) ||
             previous[0] == '_') &&
            previous != "return")
            continue;
        calls.push_back(at);
    }
    return calls;
}

// Disjuncts of every `if (...) return;` statement in [begin, use) that is
// guaranteed to have run before `use`: the guard is a statement of its own
// (not the body of an `else`, loop or other `if`) and its enclosing block is
// still open at `use`. When any returned disjunct is true, control has left
// the function before reaching `use`. Separate guards and one `||` guard are
// therefore equivalent, while `&&`, a guard in a sibling block, or a guard
// after `use` contributes nothing.
static std::set<std::string> dominating_rejections(const std::string& code,
                                                   size_t begin, size_t use) {
    struct Guard {
        int depth;
        std::vector<std::string> disjuncts;
    };
    std::vector<Guard> guards;
    int depth = 0;
    for (size_t offset = begin; offset < use && offset < code.size(); ++offset) {
        const char ch = code[offset];
        if (ch == '{') {
            ++depth;
            continue;
        }
        if (ch == '}') {
            --depth;
            while (!guards.empty() && guards.back().depth > depth)
                guards.pop_back();
            continue;
        }
        if (code.compare(offset, 2, "if") != 0) continue;
        size_t before = offset;
        while (before > begin &&
               std::isspace(static_cast<unsigned char>(code[before - 1])))
            --before;
        if (before > begin && code[before - 1] != ';' &&
            code[before - 1] != '{' && code[before - 1] != '}')
            continue;
        size_t open = offset + 2u;
        while (open < code.size() &&
               std::isspace(static_cast<unsigned char>(code[open])))
            ++open;
        if (open >= code.size() || code[open] != '(') continue;
        const size_t close = matching_closing_paren(code, open);
        if (close == std::string::npos || close >= use) continue;
        size_t statement = close + 1u;
        while (statement < code.size() &&
               std::isspace(static_cast<unsigned char>(code[statement])))
            ++statement;
        if (code.compare(statement, 7, "return;") != 0) continue;
        Guard guard{depth, {}};
        for (const std::string& disjunct :
             split_top_level(code.substr(open + 1u, close - open - 1u), "||"))
            guard.disjuncts.push_back(
                without_enclosing_parens(without_whitespace(disjunct)));
        guards.push_back(guard);
        offset = statement + 6u;
    }
    std::set<std::string> rejected;
    for (const Guard& guard : guards)
        rejected.insert(guard.disjuncts.begin(), guard.disjuncts.end());
    return rejected;
}

// Whitespace-free headers (`if(...)`, `elseif(...)`, `for(...)`) of every
// block enclosing `use`, innermost first.
static std::vector<std::string> enclosing_block_headers(const std::string& code,
                                                        size_t use) {
    std::vector<std::string> headers;
    int depth = 0;
    for (size_t offset = use; offset-- > 0;) {
        if (code[offset] == '}') {
            ++depth;
        } else if (code[offset] == '{') {
            if (depth > 0) {
                --depth;
                continue;
            }
            const size_t boundary =
                offset == 0 ? std::string::npos
                            : code.find_last_of(";{}", offset - 1u);
            const size_t header_start =
                boundary == std::string::npos ? 0 : boundary + 1u;
            headers.push_back(without_whitespace(
                code.substr(header_start, offset - header_start)));
        }
    }
    return headers;
}

static std::string mutated(const std::string& text, const std::string& from,
                           const std::string& to) {
    const size_t at = from.empty() ? std::string::npos : text.find(from);
    if (at == std::string::npos) {
        // A mutation that silently does nothing would make its negative
        // check vacuous, so a vanished anchor is a test failure in itself.
        fprintf(stderr, "shader_source_tests: mutation anchor missing: %s\n",
                from.c_str());
        std::abort();
    }
    std::string copy = text;
    copy.replace(at, from.size(), to);
    return copy;
}

// Flat-instance culling: an instance flagged as an RT-only proxy
// (`water_pad0`, set from `rt_proxy_only` by the renderer) must never reach
// raster culling, and every cluster address must be bounded by the cluster
// buffer capacity with unsigned-overflow-safe arithmetic before metadata is
// read. All rejections must dominate the flat path's cull_cluster call, and
// no early return may skip a workgroup barrier.
static bool cull_rejects_rt_proxies_and_unsafe_clusters(
    const std::string& cull_source) {
    const std::string cull = without_comments(cull_source);
    const std::string instance_body = function_body(cull, "void cull_instance()");
    const std::string cluster_body = function_body(cull, "void cull_cluster(");
    if (instance_body.empty() || cluster_body.empty()) return false;
    // main() brackets cull_instance() with barrier(); a return there is safe
    // only while the helper itself contains no barrier.
    if (instance_body.find("barrier(") != std::string::npos) return false;
    // Every instance load (hierarchy and flat) is bounded by both the live
    // count and the instance buffer capacity.
    for (size_t load = instance_body.find("instances[instance_index]");
         load != std::string::npos;
         load = instance_body.find("instances[instance_index]", load + 1u)) {
        const std::set<std::string> rejected =
            dominating_rejections(instance_body, 0, load);
        if (!rejected.count("instance_index>=frame.counts.x") ||
            !rejected.count("instance_index>=frame.capacities.y"))
            return false;
    }
    size_t flat_call = std::string::npos;
    for (size_t call : call_sites(instance_body, "cull_cluster")) {
        const std::vector<std::string> arguments =
            call_arguments(instance_body, call);
        if (arguments.size() == 4u && arguments[0] == "instance" &&
            arguments[1] == "cluster_index") {
            if (flat_call != std::string::npos) return false;
            flat_call = call;
        }
    }
    if (flat_call == std::string::npos) return false;
    // The bounded index is the one passed on, and no cluster metadata is read
    // ahead of the guarded call.
    if (without_whitespace(instance_body).find(
            "uintcluster_index=instance.cluster_start+local_cluster;") ==
        std::string::npos)
        return false;
    if (instance_body.find("clusters[") < flat_call) return false;
    const std::set<std::string> rejected =
        dominating_rejections(instance_body, 0, flat_call);
    for (const char* required : {
             "instance.water_pad0!=0u",
             "local_cluster>=instance.cluster_count",
             // Both halves: the start is in range, then the remaining
             // capacity is computed without wrapping. A single
             // `cluster_start+local_cluster>=capacity` test can wrap.
             "instance.cluster_start>=frame.capacities.x",
             "local_cluster>=frame.capacities.x-instance.cluster_start"})
        if (!rejected.count(required)) return false;
    // cull_cluster bounds its own index before the first metadata read; the
    // hierarchy path hands it indices selected on the GPU.
    const size_t first_read = cluster_body.find("clusters[");
    return first_read != std::string::npos &&
           dominating_rejections(cluster_body, 0, first_read)
               .count("cluster_index>=frame.capacities.x");
}

// geometry_select emits flat (non-hierarchy) clusters for cull.comp's
// hierarchy path; it must apply the same RT-only proxy filter before emission.
static bool geometry_select_skips_rt_proxies(const std::string& select_source) {
    const std::string body =
        function_body(without_comments(select_source), "void main()");
    const std::vector<size_t> emits = call_sites(body, "emit_cluster");
    if (emits.empty()) return false;
    for (size_t emit : emits) {
        if (call_arguments(body, emit) !=
            std::vector<std::string>{"geometry_instance.cluster_start+i"})
            return false;
        if (dominating_rejections(body, 0, emit)
                .count("geometry_instance.water_pad0!=0u"))
            continue;
        bool filtered = false;
        for (const std::string& header : enclosing_block_headers(body, emit))
            filtered |= header == "if(geometry_instance.water_pad0==0u)" ||
                        header == "}elseif(geometry_instance.water_pad0==0u)" ||
                        header == "elseif(geometry_instance.water_pad0==0u)";
        if (!filtered) return false;
    }
    return true;
}

static std::string assignment_rhs(const std::string& code,
                                  const std::string& declaration) {
    const size_t at = code.find(declaration);
    if (at == std::string::npos) return {};
    const size_t end = code.find(';', at);
    if (end == std::string::npos) return {};
    return without_whitespace(code.substr(at + declaration.size(),
                                          end - at - declaration.size()));
}

// The VT raster path hands fragments an OBJECT-space interpolated normal plus
// a flat world normal matrix (raster.vert); non-VT draws hand a world normal.
// Water forward shading must reconstruct the world-space geometric normal with
// exactly the G-buffer rule and then use only that value: as the evaluator's
// fallback normal and as the geometry-normal diagnostic.
static bool water_forward_uses_world_normal(const std::string& forward_source,
                                            const std::string& gbuffer_source) {
    const std::string forward = without_comments(forward_source);
    const std::string compact = without_whitespace(forward);
    for (const char* input : {"layout(location=0)invec3in_normal;",
                              "layout(location=8)flatinuintin_vt_slot;",
                              "layout(location=18)flatinmat3in_vt_normal_matrix;"})
        if (compact.find(input) == std::string::npos) return false;
    const std::string body = function_body(forward, "void main()");
    const std::string world_normal = assignment_rhs(body, "vec3 normal_ws =");
    if (world_normal !=
            "in_vt_slot!=0u?normalize(in_vt_normal_matrix*normalize(in_normal))"
            ":normalize(in_normal)" ||
        world_normal != assignment_rhs(without_comments(gbuffer_source),
                                       "vec3 geometric_normal_ws ="))
        return false;
    // After normal_ws exists, no consumer may fall back to the raw varying.
    const size_t world_normal_end = body.find(';', body.find("vec3 normal_ws ="));
    if (body.find("in_normal", world_normal_end) != std::string::npos)
        return false;
    const std::vector<size_t> evaluations =
        call_sites(body, "water_evaluate_surface");
    if (evaluations.size() != 1u) return false;
    const std::vector<std::string> arguments =
        call_arguments(body, evaluations[0]);
    if (arguments.size() < 5u || arguments[4] != "normal_ws") return false;
    const size_t diagnostic = body.find("== WATER_DIAGNOSTIC_GEOMETRY_NORMAL)");
    if (diagnostic == std::string::npos) return false;
    const size_t open = body.find('{', diagnostic);
    const size_t close = matching_closing_brace(body, open);
    return close != std::string::npos &&
           without_whitespace(body.substr(open, close - open + 1u)) ==
               "{color=normal_ws*0.5+0.5;}";
}

// Raster local-direct lighting never brute-forces the scene light list
// (local_light_counts.x lights). Each pixel walks either its bounded per-tile
// primary list or, as the fallback, its hashed cell span followed by every
// oversized light (local_light_counts.z of them, indexed past the span).
static bool raster_local_direct_uses_bounded_candidates(
    const std::string& composite_source) {
    const std::string code = without_comments(composite_source);
    const std::string compact = without_whitespace(code);
    const size_t define = compact.find("#defineMATTER_PRIMARY_LIGHT_CULLING1");
    const size_t include = compact.find("#include\"local_lighting.glsl\"");
    if (define == std::string::npos || include == std::string::npos ||
        define > include)
        return false;
    const std::string body = without_whitespace(
        function_body(code, "LocalLightBrdf evaluate_raster_local_direct("));
    if (body.empty() || body.find("local_light_counts.x") != std::string::npos ||
        count_occurrences(body, "for(") != 1u)
        return false;
    size_t cursor = 0;
    for (const char* step : {
             "local_light_cell_span(world_position,offset,count);",
             "uintlist_base=primary_light_tile_list_base(tile_base,list_count);",
             "uintcandidate_count=list_base!=0xffffffffu?list_count"
             ":count+local_light_counts.z;",
             "for(uintcandidate=0u;candidate<candidate_count;++candidate){",
             "uintindex=list_base!=0xffffffffu"
             "?local_primary_masks[list_base+candidate]"
             ":(candidate<count?local_light_indices[offset+candidate]"
             ":local_light_oversized_indices[candidate-count]);",
             "evaluate_local_light_brdf(index,"}) {
        cursor = body.find(step, cursor);
        if (cursor == std::string::npos) return false;
    }
    return true;
}

// A shader entry plus every file reached through quoted #include directives,
// as paths relative to MatterEngine3 and as one concatenated text.
struct ShaderClosure {
    std::set<std::string> files;
    std::string text;
    bool complete = true;
};

static void collect_shader_closure(const std::filesystem::path& relative,
                                   ShaderClosure& closure) {
    const std::string key = relative.lexically_normal().generic_string();
    if (!closure.files.insert(key).second) return;
    const std::string source = read_shader(("../" + key).c_str());
    if (source.empty()) {
        closure.complete = false;
        return;
    }
    closure.text += source;
    closure.text += '\n';
    const std::string code = without_comments(source);
    for (size_t line_start = 0; line_start < code.size();) {
        size_t line_end = code.find('\n', line_start);
        if (line_end == std::string::npos) line_end = code.size();
        const std::string line = code.substr(line_start, line_end - line_start);
        if (without_whitespace(line).rfind("#include\"", 0) == 0) {
            const size_t quote = line.find('"');
            const size_t end_quote = line.find('"', quote + 1u);
            if (end_quote == std::string::npos)
                closure.complete = false;
            else
                collect_shader_closure(
                    relative.parent_path() /
                        line.substr(quote + 1u, end_quote - quote - 1u),
                    closure);
        }
        line_start = line_end + 1u;
    }
}

static ShaderClosure shader_closure(const std::string& entry) {
    ShaderClosure closure;
    collect_shader_closure(entry, closure);
    return closure;
}

// An RT lighting entry point is a specialization of the one shared
// implementation: only preprocessor lines, exactly one include of the
// implementation, and no private main() or private water evaluator.
static bool rt_entry_specializes_shared_impl(const std::string& wrapper_source,
                                             const std::string& impl_source) {
    const std::string wrapper = without_comments(wrapper_source);
    for (size_t line_start = 0; line_start < wrapper.size();) {
        size_t line_end = wrapper.find('\n', line_start);
        if (line_end == std::string::npos) line_end = wrapper.size();
        const std::string line =
            without_whitespace(wrapper.substr(line_start, line_end - line_start));
        if (!line.empty() && line[0] != '#') return false;
        line_start = line_end + 1u;
    }
    return count_occurrences(without_whitespace(wrapper),
                             "#include\"rt_lighting_impl.glsl\"") == 1u &&
           count_occurrences(without_whitespace(without_comments(impl_source)),
                             "voidmain(") == 1u;
}

// Local-light visibility rays start from the undisplaced proxy/TLAS surface
// while BRDF evaluation uses the displaced shading receiver. Every helper must
// forward both positions unchanged, and each call site of
// evaluate_rt_local_lighting must pass the matching pair:
//   primary   : shading_world (G-buffer/POM receiver), world (lifted origin)
//   secondary : surface.position, surface.visibility_position
//   hit       : hit.surface.position, hit.surface.visibility_position
// The primary geometric normal is the proxy normal only for surface detail.
static bool rt_local_lighting_splits_receiver_and_origin(
    const std::string& impl_source) {
    const std::string impl = without_comments(impl_source);
    const std::vector<size_t> visibility_calls =
        call_sites(impl, "local_light_visibility");
    if (visibility_calls.empty()) return false;
    for (size_t call : visibility_calls) {
        const std::vector<std::string> arguments = call_arguments(impl, call);
        if (arguments.size() != 5u || arguments[1] != "receiver_position" ||
            arguments[2] != "ray_origin_position")
            return false;
    }
    const std::string helper =
        function_body(impl, "vec3 local_light_visibility(LocalLightGpu light,");
    if (without_whitespace(helper).find(
            "vec3origin=ray_origin_position+receiver_normal*constants.bias;") ==
        std::string::npos)
        return false;
    std::map<std::string, std::vector<std::string>> by_receiver;
    for (size_t call : call_sites(impl, "evaluate_rt_local_lighting")) {
        const std::vector<std::string> arguments = call_arguments(impl, call);
        if (arguments.size() < 4u ||
            !by_receiver.emplace(arguments[0], arguments).second)
            return false;
    }
    const std::map<std::string, std::vector<std::string>> expected = {
        {"shading_world",
         {"shading_world", "world", "direct_normal",
          "primary_surface_detail?primary_geo_normal:direct_normal"}},
        {"surface.position",
         {"surface.position", "surface.visibility_position", "shading_normal",
          "surface.normal"}},
        {"hit.surface.position",
         {"hit.surface.position", "hit.surface.visibility_position",
          "hit_shading_normal", "hit.surface.normal"}}};
    if (by_receiver.size() != expected.size()) return false;
    for (const auto& [receiver, prefix] : expected) {
        const auto call = by_receiver.find(receiver);
        if (call == by_receiver.end() ||
            !std::equal(prefix.begin(), prefix.end(), call->second.begin()))
            return false;
    }
    return true;
}

// Normal prerequisites of every explicit build/shaders_vk/*.spv rule, merged
// per target. `$(wildcard shaders_vk/*.glsl)` is kept as the pattern
// `shaders_vk/*.glsl`; order-only prerequisites never trigger a rebuild and
// are dropped.
static std::map<std::string, std::set<std::string>> spirv_prerequisites(
    const std::string& makefile) {
    std::map<std::string, std::set<std::string>> rules;
    auto tokens = [](const std::string& text) {
        std::vector<std::string> words;
        std::string word;
        for (char ch : text + " ") {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word += ch;
            }
        }
        return words;
    };
    auto add_rule = [&](std::string logical) {
        logical = logical.substr(0, logical.find('#'));
        const std::string wildcard = "$(wildcard shaders_vk/*.glsl)";
        for (size_t at = logical.find(wildcard); at != std::string::npos;
             at = logical.find(wildcard, at))
            logical.replace(at, wildcard.size(), "shaders_vk/*.glsl");
        const size_t colon = logical.find(':');
        if (colon == std::string::npos ||
            logical.find('=') < colon + 2u)  // :=, ::=, = and target vars
            return;
        std::string prerequisites = logical.substr(colon + 1u);
        prerequisites = prerequisites.substr(0, prerequisites.find('|'));
        for (const std::string& target : tokens(logical.substr(0, colon))) {
            if (target.rfind("build/shaders_vk/", 0) != 0 ||
                target.size() < 4u ||
                target.compare(target.size() - 4u, 4u, ".spv") != 0 ||
                target.find('%') != std::string::npos)
                continue;
            for (const std::string& prerequisite : tokens(prerequisites))
                rules[target].insert(prerequisite);
        }
    };
    std::string logical;
    for (size_t line_start = 0; line_start < makefile.size();) {
        size_t line_end = makefile.find('\n', line_start);
        if (line_end == std::string::npos) line_end = makefile.size();
        std::string line = makefile.substr(line_start, line_end - line_start);
        line_start = line_end + 1u;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (logical.empty() && !line.empty() && line[0] == '\t')
            continue;  // recipe
        const bool continued = !line.empty() && line.back() == '\\';
        if (continued) line.pop_back();
        logical += line;
        logical += ' ';
        if (continued) continue;
        add_rule(logical);
        logical.clear();
    }
    if (!logical.empty()) add_rule(logical);
    return rules;
}

// The Make build must rebuild an entry's SPIR-V whenever any file in its
// include closure changes, or it silently embeds stale shader code.
static bool spirv_rule_covers_includes(
    const std::map<std::string, std::set<std::string>>& rules,
    const std::string& entry, std::string& missing) {
    const ShaderClosure closure = shader_closure(entry);
    if (!closure.complete) {
        missing = entry + " include closure is unreadable";
        return false;
    }
    static const std::set<std::string> none;
    const auto rule = rules.find("build/" + entry + ".spv");
    const std::set<std::string>& prerequisites =
        rule == rules.end() ? none : rule->second;
    for (const std::string& dependency : closure.files) {
        if (dependency == entry || prerequisites.count(dependency)) continue;
        const bool glsl_in_shader_dir =
            dependency.rfind("shaders_vk/", 0) == 0 &&
            dependency.find('/', 11u) == std::string::npos &&
            dependency.size() > 5u &&
            dependency.compare(dependency.size() - 5u, 5u, ".glsl") == 0;
        if (glsl_in_shader_dir && prerequisites.count("shaders_vk/*.glsl"))
            continue;
        missing = "build/" + entry + ".spv lacks " + dependency;
        return false;
    }
    return true;
}

int main() {
    std::string text, err;
    // 1. embedded lookup works and matches the on-disk source.
    // shaders_gpu/cull.comp (the GL GpuCuller compute shader) is gone --
    // its only readers were deleted in Phase 5a (tech-debt.md §6) and it was
    // dropped from SHADER_LOGICAL in MatterEngine3/Makefile -- so this
    // round-trip check uses shaders/bvh_tlas_common.glsl, a still-embedded
    // fixture, instead.
    assert(matter::shader_text("shaders/bvh_tlas_common.glsl", text, err));
    assert(text.find("layout") != std::string::npos);
    // 2. unknown path fails with a useful error
    std::string t2;
    assert(!matter::shader_text("shaders/nope.fs", t2, err));
    assert(err.find("nope.fs") != std::string::npos);
    // 3. override dir wins: write a marker file, point the override at it
    const std::filesystem::path override_dir =
        std::filesystem::absolute("build/shader_override_test");
    std::filesystem::create_directories(override_dir / "shaders");
    std::ofstream override_file(override_dir / "shaders/bvh_tlas_common.glsl");
    override_file << "// OVERRIDE MARKER\n";
    override_file.close();
    matter::set_shader_override_dir(override_dir.string().c_str());
    std::string t3;
    assert(matter::shader_text("shaders/bvh_tlas_common.glsl", t3, err));
    assert(t3.find("OVERRIDE MARKER") != std::string::npos);
    matter::set_shader_override_dir(nullptr);

    // Baked water animation stays packed through the GPU upload. The water
    // vertex specialization decodes the 12-byte file ABI directly, avoiding
    // a full-frame compute expansion and its device-memory round trip.
    const std::string raster_vertex =
        read_shader("../shaders_vk/raster.vert");
    const std::string renderer_header =
        read_shader("../src/render/vk_scene_renderer.h");
    const std::string renderer_source =
        read_shader("../src/render/vk_scene_renderer.cpp");
    const std::string water_animation_header =
        read_shader("../src/render/water_animation_gpu.h");
    const std::string root_cmake = read_shader("../../CMakeLists.txt");
    const std::string engine_makefile = read_shader("../Makefile");
    assert(raster_vertex.find("MATTER_WATER_ANIMATION_VERTEX_INPUT") !=
           std::string::npos);
    assert(raster_vertex.find("uvec3 in_water_packed") != std::string::npos);
    assert(raster_vertex.find("decode_water_octahedral") != std::string::npos);
    assert(raster_vertex.find("debug_push.water_bounds_min") !=
           std::string::npos);
    // RT-only proxies (water_pad0 != 0: animated-water static RT proxies and
    // geometry-hierarchy ready-part proxies) must never be raster-culled, and
    // cluster addressing must stay bounded. The rejection may be one
    // disjunction or separate early returns; what matters is that each
    // condition dominates the cull_cluster call.
    const std::string cull_shader = read_shader("../shaders_vk/cull.comp");
    assert(cull_rejects_rt_proxies_and_unsafe_clusters(cull_shader));
    const std::string flat_guard =
        "if (instance.water_pad0 != 0u || local_cluster >= instance.cluster_count ||";
    const std::string flat_bounds =
        "instance.cluster_start >= frame.capacities.x || local_cluster >= "
        "frame.capacities.x-instance.cluster_start) return;";
    const std::string flat_call =
        "cull_cluster(instance, cluster_index, clusters[cluster_index].part_slot, "
        "0xffffffffu);";
    // Proxy filter dropped.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(
        mutated(cull_shader, "instance.water_pad0 != 0u || ", "")));
    // Rejection weakened from "any" to "all".
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        cull_shader, "instance.water_pad0 != 0u ||", "instance.water_pad0 != 0u &&")));
    // Proxy filter split out but placed after the cull_cluster call.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        mutated(cull_shader, "instance.water_pad0 != 0u || ", ""), flat_call,
        flat_call + "\n    if (instance.water_pad0 != 0u) return;")));
    // Proxy filter split out and placed in a sibling block that has closed.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        mutated(cull_shader, "instance.water_pad0 != 0u || ", ""),
        "GpuInstance instance = instances[instance_index];",
        "GpuInstance instance = instances[instance_index];\n"
        "    if (frame.counts.z != 0u) { if (instance.water_pad0 != 0u) return; }")));
    // The same filter as its own statement is equivalent and must pass.
    assert(cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        mutated(cull_shader, "instance.water_pad0 != 0u || ", ""),
        "GpuInstance instance = instances[instance_index];",
        "GpuInstance instance = instances[instance_index];\n"
        "    if (instance.water_pad0 != 0u) return;")));
    // Range check rewritten in the wrapping form start + local >= capacity.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        cull_shader, flat_bounds,
        "instance.cluster_start+local_cluster >= frame.capacities.x) return;")));
    const std::string full_flat_guard = flat_guard + "\n        " + flat_bounds;
    // Guard commented out.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(
        mutated(cull_shader, full_flat_guard, "/* " + full_flat_guard + " */")));
    // Whole guard moved below the call.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        mutated(cull_shader, full_flat_guard, ""), flat_call,
        flat_call + "\n    " + full_flat_guard)));
    // cull_cluster no longer bounds the GPU-selected hierarchy index.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        cull_shader, "if (cluster_index >= frame.capacities.x) return;", "")));
    // Flat instance load no longer bounded by the instance capacity.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        cull_shader,
        "if (instance_index >= frame.counts.x || instance_index >= "
        "frame.capacities.y) return;\n    GpuInstance",
        "if (instance_index >= frame.counts.x) return;\n    GpuInstance")));
    // An early return that skips a workgroup barrier.
    assert(!cull_rejects_rt_proxies_and_unsafe_clusters(mutated(
        cull_shader, "if (frame.counts.y == 0u) return;",
        "barrier();\n    if (frame.counts.y == 0u) return;")));
    // geometry_select feeds the hierarchy cull path, including flat jobs.
    const std::string geometry_select_shader =
        read_shader("../shaders_vk/geometry_select.comp");
    assert(geometry_select_skips_rt_proxies(geometry_select_shader));
    assert(!geometry_select_skips_rt_proxies(mutated(
        geometry_select_shader, "} else if (geometry_instance.water_pad0 == 0u) {",
        "} else {")));
    assert(!geometry_select_skips_rt_proxies(mutated(
        geometry_select_shader, "} else if (geometry_instance.water_pad0 == 0u) {",
        "} else if (geometry_instance.water_pad0 != 0u) {")));
    assert(geometry_select_skips_rt_proxies(mutated(
        mutated(geometry_select_shader,
                "} else if (geometry_instance.water_pad0 == 0u) {", "} else {"),
        "geometry_instance = instances[geometry_instance_index];",
        "geometry_instance = instances[geometry_instance_index];\n"
        "    if (geometry_instance.water_pad0 != 0u) return;")));
    assert(root_cmake.find(
               "raster_water.vert|raster.vert|MATTER_WATER_ANIMATION_VERTEX_INPUT") !=
           std::string::npos);
    assert(engine_makefile.find("water_animation_decode.comp.spv") ==
               std::string::npos);
    assert(engine_makefile.find("water_animation_rt_decode.comp.spv") ==
               std::string::npos);
    assert(renderer_source.find("prepare_water_animation_blas") ==
               std::string::npos);
    assert(renderer_header.find("water_animation_current_blas_") ==
               std::string::npos);
    assert(water_animation_header.find("WaterAnimationRtDecodeHeader") ==
               std::string::npos);
    assert(engine_makefile.find("raster_water.vert.spv") !=
           std::string::npos);
    const size_t water_forward_inventory = engine_makefile.find(
        "build/shaders_vk/water_forward.frag.spv");
    assert(water_forward_inventory != std::string::npos);
    const size_t water_forward_dependencies = engine_makefile.find(
        "build/shaders_vk/water_forward.frag.spv:");
    assert(water_forward_dependencies != std::string::npos);
    const std::string water_forward_dependency_rule =
        engine_makefile.substr(water_forward_dependencies, 320);
    for (const char* dependency : {"shaders_vk/material_common.glsl",
                                   "shaders_vk/water_surface.glsl",
                                   "shaders_vk/water_screen_space.glsl",
                                   "shaders_vk/environment_common.glsl",
                                   "shaders_vk/cloud_shadow_common.glsl"})
        assert(water_forward_dependency_rule.find(dependency) !=
               std::string::npos);
    const std::string water_forward =
        read_shader("../shaders_vk/water_forward.frag");
    const std::string water_screen =
        read_shader("../shaders_vk/water_screen_space.glsl");
    assert(water_forward.find("water_refract_scene") != std::string::npos);
    assert(water_forward.find("water_reflect_scene") != std::string::npos);
    assert(water_forward.find("water_evaluate_optics") != std::string::npos);
    assert(water_screen.find("const int WATER_REFLECTION_STEPS = 24;") !=
           std::string::npos);
    assert(water_screen.find(
               "const int WATER_REFLECTION_REFINEMENT_STEPS = 4;") !=
           std::string::npos);
    assert(water_screen.find(
               "step < WATER_REFLECTION_STEPS") != std::string::npos);
    assert(water_screen.find(
               "refinement < WATER_REFLECTION_REFINEMENT_STEPS") !=
           std::string::npos);
    const size_t reflection_march = water_screen.find(
        "for (int step = 0; step < WATER_REFLECTION_STEPS; ++step)");
    const size_t reflection_march_open =
        water_screen.find('{', reflection_march);
    const size_t reflection_march_close =
        matching_closing_brace(water_screen, reflection_march_open);
    const size_t reflection_refinement = water_screen.find(
        "for (int refinement = 0;");
    // Brace-scope the march rather than merely counting loop tokens: the
    // single refinement phase must be after the march has exited, so a failed
    // refined candidate cannot resume marching and refine a second bracket.
    assert(reflection_march != std::string::npos &&
           reflection_march_close != std::string::npos &&
           reflection_refinement != std::string::npos &&
           reflection_refinement > reflection_march_close);
    assert(count_occurrences(water_screen,
                             "for (int refinement = 0;") == 1u);
    const size_t missing_bracket_miss = water_screen.find(
        "if (!reflection_bracket_valid)", reflection_march_close);
    const size_t invalid_refinement_miss = water_screen.find(
        "if (!refinement_valid ||", reflection_refinement);
    const size_t accepted_reflection = water_screen.find(
        "result.valid = true;", reflection_refinement);
    assert(missing_bracket_miss > reflection_march_close &&
           missing_bracket_miss < reflection_refinement &&
           invalid_refinement_miss > reflection_refinement &&
           accepted_reflection > invalid_refinement_miss);
    const size_t refraction_helper =
        water_screen.find("vec2 water_refraction_uv(");
    const size_t refraction_helper_open =
        water_screen.find('{', refraction_helper);
    const size_t refraction_helper_close =
        matching_closing_brace(water_screen, refraction_helper_open);
    assert(refraction_helper != std::string::npos &&
           refraction_helper_close != std::string::npos);
    const std::string refraction_contract = water_screen.substr(
        refraction_helper, refraction_helper_close - refraction_helper);
    assert(water_screen.find(
               "water_refraction_uv(result.uv, normal.xz,") !=
           std::string::npos);
    assert(refraction_contract.find(
               "normal_xz * max(optical_distance_m, 0.0)") !=
           std::string::npos);
    assert(refraction_contract.find("water_clamp_pixel_offset(") !=
               std::string::npos &&
           refraction_contract.find(
               "water_forward.viewport_refraction.z") !=
               std::string::npos);
    assert(refraction_contract.find(
               "source_uv + offset_px / viewport") !=
           std::string::npos);
    assert(water_screen.find("water_world + normal") ==
           std::string::npos);
    assert(water_forward.find("sample_physical_sky") != std::string::npos);
    for (const char* forbidden : {"visibility_texture", "raw_diffuse",
                                  "raw_specular", "raw_transmission",
                                  "accelerationStructureEXT", "rayQuery",
                                  "traceRayEXT", "topLevelAS", "tlas",
                                  "sample_cloud_transmittance",
                                  "GL_EXT_ray_query",
                                  "GL_EXT_ray_tracing"}) {
        assert(water_forward.find(forbidden) == std::string::npos);
        assert(water_screen.find(forbidden) == std::string::npos);
    }
    assert(renderer_header.find("kGpuZoneWaterDecode") != std::string::npos);
    assert(renderer_header.find("kGpuZoneWaterDraw") != std::string::npos);
    assert(renderer_source.find(
               "water_forward.timing_zone = kGpuZoneWaterDraw") !=
           std::string::npos);
    assert(renderer_source.find("kGpuZoneWaterDecode") ==
           std::string::npos);

    // River presentation Task 8: raster and RT must share one bounded water
    // evaluator. Private copies inevitably drift at reset boundaries and make
    // the static mesh appear different in reflections than in the G-buffer.
    const std::string water_common =
        read_shader("../shaders_vk/water_surface.glsl");
    const std::string water_raster =
        read_shader("../shaders_vk/gbuffer.frag");
    // The RT lighting entry points are thin specializations of one shared
    // implementation (rt_lighting_impl.glsl owns main()), so RT water
    // contracts are stated against that implementation and every entry is
    // checked to reach it.
    const std::string rt_impl_path = "../shaders_vk/rt_lighting_impl.glsl";
    const std::string water_rt = read_shader(rt_impl_path.c_str());
    std::vector<std::string> rt_lighting_entries;
    for (const auto& entry :
         std::filesystem::directory_iterator("../shaders_vk")) {
        if (entry.path().extension() != ".rgen") continue;
        const std::string relative =
            "shaders_vk/" + entry.path().filename().generic_string();
        if (read_shader(("../" + relative).c_str())
                .find("rt_lighting_impl.glsl") != std::string::npos)
            rt_lighting_entries.push_back(relative);
    }
    std::sort(rt_lighting_entries.begin(), rt_lighting_entries.end());
    assert(!water_rt.empty());
    assert(std::count(rt_lighting_entries.begin(), rt_lighting_entries.end(),
                      "shaders_vk/rt_lighting.rgen") == 1);
    assert(std::count(rt_lighting_entries.begin(), rt_lighting_entries.end(),
                      "shaders_vk/rt_primary_adaptive.rgen") == 1);
    for (const std::string& entry : rt_lighting_entries) {
        const std::string wrapper = read_shader(("../" + entry).c_str());
        assert(rt_entry_specializes_shared_impl(wrapper, water_rt));
        // Each entry compiles exactly the shared evaluator and nothing
        // private: its whole include closure defines water_backtrace_rk2 once.
        const ShaderClosure closure = shader_closure(entry);
        assert(closure.complete);
        assert(closure.files.count("shaders_vk/rt_lighting_impl.glsl") == 1u);
        assert(closure.files.count("shaders_vk/water_surface.glsl") == 1u);
        assert(count_occurrences(without_comments(closure.text),
                                 "vec2 water_backtrace_rk2(") == 1u);
    }
    const std::string rt_wrapper = read_shader("../shaders_vk/rt_lighting.rgen");
    assert(!rt_entry_specializes_shared_impl(
        mutated(rt_wrapper, "#include \"rt_lighting_impl.glsl\"", ""), water_rt));
    assert(!rt_entry_specializes_shared_impl(
        mutated(rt_wrapper, "#include \"rt_lighting_impl.glsl\"",
                "// #include \"rt_lighting_impl.glsl\""),
        water_rt));
    assert(!rt_entry_specializes_shared_impl(
        rt_wrapper + "\nvoid main() {}\n", water_rt));
    assert(!rt_entry_specializes_shared_impl(
        rt_wrapper + "\n#include \"rt_lighting_impl.glsl\"\n", water_rt));
    assert(!rt_entry_specializes_shared_impl(
        rt_wrapper, mutated(water_rt, "void main()", "void rt_main()")));
    const std::string water_visibility =
        read_shader("../shaders_vk/rt_visibility.rahit");
    const std::string primary_shadow =
        read_shader("../shaders_vk/rt_shadow.rgen");
    assert(!water_common.empty());
    assert(water_raster.find("#include \"water_surface.glsl\"") !=
           std::string::npos);
    assert(renderer_header.find("uint32_t diagnostics[4]") !=
           std::string::npos);
    assert(water_screen.find("uvec4 diagnostics;") != std::string::npos);
    assert(raster_vertex.find(
               "layout(location = 17) flat out uint out_water_diagnostic_identity") !=
           std::string::npos);
    assert(water_raster.find(
               "layout(location = 17) flat in uint in_water_diagnostic_identity") !=
           std::string::npos);
    assert(water_forward.find(
               "layout(location = 17) flat in uint in_water_diagnostic_identity") !=
           std::string::npos);
    assert(raster_vertex.find("uint water_diagnostic_identity;") !=
           std::string::npos);
    assert(water_raster.find("uint water_diagnostic_identity;") !=
           std::string::npos);
    assert(water_forward.find("WATER_DIAGNOSTIC_IDENTITY") !=
               std::string::npos &&
           water_forward.find("WATER_DIAGNOSTIC_GEOMETRY_NORMAL") !=
               std::string::npos &&
           water_forward.find("WATER_DIAGNOSTIC_FOAM_DRIVER") !=
               std::string::npos);
    const size_t diagnostic_branch = water_forward.find(
        "if (diagnostic_view != WATER_DIAGNOSTIC_NONE)");
    const size_t normal_optics_branch = water_forward.find(
        "else if (surface_valid)", diagnostic_branch);
    const size_t first_optics_sample = water_forward.find(
        "water_refract_scene", diagnostic_branch);
    assert(diagnostic_branch != std::string::npos &&
           normal_optics_branch != std::string::npos &&
           first_optics_sample > normal_optics_branch);
    // The geometry-normal diagnostic and the evaluator fallback consume the
    // reconstructed WORLD-space normal. VT draws interpolate an object-space
    // in_normal (raster.vert), so visualizing or shading with the raw varying
    // is wrong for every rotated VT water instance.
    assert(water_forward_uses_world_normal(water_forward, water_raster));
    assert(without_whitespace(raster_vertex).find(
               "out_normal=draw.vt_slot!=0u?in_normal:normalize(mat3(model)*in_normal);") !=
           std::string::npos);
    assert(without_whitespace(raster_vertex).find(
               "out_vt_normal_matrix=vt_instance_normal_matrix(mat3(model));") !=
           std::string::npos);
    assert(without_whitespace(raster_vertex).find(
               "layout(location=8)flatoutuintout_vt_slot;") != std::string::npos &&
           without_whitespace(raster_vertex).find(
               "layout(location=18)flatoutmat3out_vt_normal_matrix;") !=
               std::string::npos);
    assert(!water_forward_uses_world_normal(
        mutated(water_forward, "color = normal_ws * 0.5 + 0.5;",
                "color = normalize(in_normal) * 0.5 + 0.5;"),
        water_raster));
    assert(!water_forward_uses_world_normal(
        mutated(water_forward, "in_world_pos.xz, normal_ws,",
                "in_world_pos.xz, normalize(in_normal),"),
        water_raster));
    assert(!water_forward_uses_world_normal(
        mutated(water_forward,
                "in_vt_slot != 0u\n        ? normalize(in_vt_normal_matrix * normalize(in_normal))\n        : normalize(in_normal);",
                "normalize(in_normal);"),
        water_raster));
    assert(!water_forward_uses_world_normal(
        mutated(water_forward, "normalize(in_vt_normal_matrix * normalize(in_normal))",
                "normalize(transpose(in_vt_normal_matrix) * normalize(in_normal))"),
        water_raster));
    // The G-buffer and the forward water path must agree on the rule.
    assert(!water_forward_uses_world_normal(
        water_forward,
        mutated(water_raster, "normalize(in_vt_normal_matrix * normalize(in_normal))",
                "normalize(in_normal)")));
    assert(water_forward.find(
               "surface_valid ? water_foam_driver_heatmap(surface.foam.coverage) : vec3(0.0)") !=
           std::string::npos);
    assert(water_common.find("vec3 water_apply_foam_radiance") !=
           std::string::npos);
    assert(water_forward.find(
               "water_apply_foam_radiance(color, surface.foam.coverage)") !=
           std::string::npos);
    assert(water_rt.find("#include \"water_surface.glsl\"") !=
           std::string::npos);
    assert(count_occurrences(water_common, "vec2 water_backtrace_rk2(") == 1u);
    assert(count_occurrences(water_raster, "water_backtrace_rk2(") == 0u);
    assert(count_occurrences(water_rt, "water_backtrace_rk2(") == 0u);
    assert(water_common.find("const int WATER_BACKTRACE_STEPS = 3;") !=
           std::string::npos);
    assert(water_common.find("const int WATER_WAVE_BAND_COUNT = 3;") !=
           std::string::npos);
    assert(water_common.find("const int WATER_PHASE_COUNT = 2;") !=
           std::string::npos);
    assert(water_common.find("const int WATER_FIELD_FRINGE_RADIUS = 2;") !=
           std::string::npos);
    assert(water_common.find("water_field_record_matches") !=
           std::string::npos);
    assert(water_common.find("water_default_fringe_sample") !=
           std::string::npos);
    assert(water_common.find("water_field_a[nonuniformEXT") !=
           std::string::npos);
    assert(water_common.find("water_field_c[nonuniformEXT") !=
           std::string::npos);
    assert(water_common.find(
               "for (int step = 0; step < WATER_BACKTRACE_STEPS; ++step)") !=
           std::string::npos);
    assert(water_common.find(
               "for (int band = 0; band < WATER_WAVE_BAND_COUNT; ++band)") !=
           std::string::npos);
    assert(water_common.find("uniform sampler2D water_field_d") !=
           std::string::npos);
    assert(water_common.find("optics_shallow") != std::string::npos &&
           water_common.find("foam_controls") != std::string::npos &&
           water_common.find("water_breakup_noise") != std::string::npos);
    assert(water_raster.find("out_reactivity") != std::string::npos);
    assert(water_forward.find("bool surface_valid") != std::string::npos);
    assert(water_forward.find("else if (surface_valid)") !=
           std::string::npos);
    const size_t material_lookup = water_common.find(
        "bool water_evaluate_surface_for_material(");
    const size_t material_lookup_open =
        water_common.find('{', material_lookup);
    const size_t material_lookup_close =
        matching_closing_brace(water_common, material_lookup_open);
    const std::string material_lookup_body =
        material_lookup != std::string::npos &&
                material_lookup_close != std::string::npos
            ? water_common.substr(material_lookup,
                                  material_lookup_close - material_lookup + 1u)
            : std::string{};
    const size_t material_wet_support = material_lookup_body.find(
        "water_sample_field(slot, record.extent_generation.z, material_id");
    const size_t material_surface_evaluation = material_lookup_body.find(
        "water_evaluate_surface(");
    assert(material_wet_support != std::string::npos &&
           material_surface_evaluation != std::string::npos &&
           material_wet_support < material_surface_evaluation);
    // A closed water mesh contributes an entry and exit any-hit to a sun
    // shadow ray.  Its dark display albedo is not an absorption coefficient:
    // using it as the tint twice blackens the riverbed even when the authored
    // shallow optics are almost clear.  Water shadows use the material's
    // optical absorption lane (with the same black-means-clear compatibility
    // rule as the refraction walk); ordinary colored glass stays unchanged.
    assert(water_visibility.find("WATER_SURFACE_MATERIAL_FLAG") !=
           std::string::npos);
    assert(water_visibility.find("material.absorption_pad.rgb") !=
           std::string::npos);
    assert(water_visibility.find("water_shadow_absorption") !=
           std::string::npos);
    // Transparent water has its own RT transmission walk, which shades the
    // terrain below the surface (including sun visibility).  The primary
    // shadow target must stay neutral for water receivers or the same caster
    // appears once on the surface and again on the riverbed.
    assert(primary_shadow.find("is_water_receiver") != std::string::npos);
    assert(primary_shadow.find(
               "if (is_water_receiver) {\n"
               "        imageStore(visibility_image, pixel, vec4(1.0));\n"
               "        return;\n"
               "    }") != std::string::npos);
    const std::string gi_temporal =
        read_shader("../shaders_vk/gi_temporal.comp");
    const std::string gi_atrous =
        read_shader("../shaders_vk/gi_atrous.comp");
    assert(gi_temporal.find("reactivityTex") != std::string::npos &&
           gi_temporal.find("float alpha = mix") != std::string::npos);
    assert(gi_atrous.find("reactivityTex") != std::string::npos &&
           gi_atrous.find("float wr = exp") != std::string::npos);

    // Task 7: every production lighting consumer must use the shared physical
    // environment path; a procedural fallback would make raster/RT/fog diverge.
    // Checked over each entry's full include closure: with RT lighting
    // factored into rt_lighting_impl.glsl, scanning only the thin entry files
    // would let a private sky model return unnoticed through any include.
    std::vector<std::string> entries = {"shaders_vk/composite.frag",
                                        "shaders_vk/vol_scatter.comp"};
    entries.insert(entries.end(), rt_lighting_entries.begin(),
                   rt_lighting_entries.end());
    std::map<std::string, std::string> production_closure;
    for (const std::string& entry : entries) {
        const ShaderClosure closure = shader_closure(entry);
        assert(closure.complete && !closure.text.empty());
        production_closure[entry] = closure.text;
        const std::string& production = closure.text;
        assert(production.find("procedural_sky") == std::string::npos);
        assert(production.find("sky_with_sun") == std::string::npos);
        assert(production.find("#include \"sky_common.glsl\"") == std::string::npos);
        assert(closure.files.count("shaders_vk/sky_common.glsl") == 0u);
    }
    // sky_view stores azimuth relative to the sun, while its SH output is
    // evaluated later with world-space normals. The producer must rotate its
    // quadrature direction into world space before evaluating the basis.
    const std::string irradiance =
        read_shader("../shaders_vk/atmosphere_irradiance.comp");
    assert(irradiance.find("vec3 world_direction") != std::string::npos);
    assert(irradiance.find("sh(int(coefficient),world_direction)") !=
           std::string::npos);
    const std::string environment =
        read_shader("../shaders_vk/environment_common.glsl");
    assert(environment.find("#ifndef ENVIRONMENT_SET") !=
           std::string::npos);
    assert(environment.find("#define ENVIRONMENT_SET 0") !=
           std::string::npos);
    assert(environment.find("#define ENVIRONMENT_SET 1") !=
           std::string::npos);
    assert(environment.find("layout(set = ENVIRONMENT_SET, binding = 0) uniform sampler2D atmosphere_sky_view") !=
           std::string::npos);
    assert(environment.find("layout(set = ENVIRONMENT_SET, binding = 6, std140) uniform EnvironmentBlock") !=
           std::string::npos);
    assert(environment.find("sample_sky_irradiance") != std::string::npos &&
           environment.find("environment.cloud_state.x == 0.0") !=
               std::string::npos);
    assert(environment.find("fract(azimuth_u)") != std::string::npos);
    assert(environment.find("clamp(v, 0.5 / 108.0, 107.5 / 108.0)") !=
           std::string::npos);
    // Task 2: visible sky, post-9SH irradiance, direct-world sunlight and the
    // analytic disc are four independent CPU-resolved lanes in one UBO.  None
    // of the three consumers may retain its former private sun/sky colours or
    // reconstruct an elevation curve in GLSL.
    for (const char* name : {"vec4 direct_world_sun_ratio;",
                             "vec4 sun_disc_reserved;",
                             "vec4 sky_display_reserved;",
                             "vec4 sky_irradiance_ambient_ratio;"})
        assert(environment.find(name) != std::string::npos);
    const size_t sh_loop = environment.find(
        "for (int coefficient = 0; coefficient < 9; ++coefficient)");
    const size_t irradiance_modifier = environment.find(
        "environment.sky_irradiance_ambient_ratio.rgb", sh_loop);
    assert(sh_loop != std::string::npos && irradiance_modifier != std::string::npos &&
           irradiance_modifier > sh_loop);
    assert(environment.find("environment.sky_display_reserved.rgb") !=
           std::string::npos);
    for (const std::string& entry : entries) {
        const std::string& production = production_closure[entry];
        for (const char* forbidden : {"lighting.sky_color", "lighting.sun_color",
                                      "constants.sky_color", "constants.sun_color",
                                      "pc.sky_color", "pc.sun_color",
                                      "smoothstep(-6", "smoothstep(0,5",
                                      "elevation_deg"})
            assert(production.find(forbidden) == std::string::npos);
    }
    const std::string composite = read_shader("../shaders_vk/composite.frag");
    const std::string rt = water_rt;  // the shared RT lighting implementation
    const std::string volume = read_shader("../shaders_vk/vol_scatter.comp");
    const std::string local_lighting =
        read_shader("../shaders_vk/local_lighting.glsl");
    assert(!local_lighting.empty());
    for (const char* binding : {
             "layout(set = 2, binding = 0, std430)",
             "layout(set = 2, binding = 1, std430)",
             "layout(set = 2, binding = 2, std430)",
             "layout(set = 2, binding = 3, std430)",
             "layout(set = 2, binding = 4, std430)"})
        assert(local_lighting.find(binding) != std::string::npos);
    assert(local_lighting.find("0x8da6b343u") != std::string::npos &&
           local_lighting.find("0xd8163841u") != std::string::npos &&
           local_lighting.find("0xcb1ab31fu") != std::string::npos);
    assert(local_lighting.find("uvec3 reserved;") == std::string::npos &&
           local_lighting.find("uint reserved0;") != std::string::npos &&
           local_lighting.find("uint reserved1;") != std::string::npos &&
           local_lighting.find("uint reserved2;") != std::string::npos);
    assert(local_lighting.find("normalized_delta = delta / range") !=
               std::string::npos &&
           local_lighting.find("cutoff * cutoff") != std::string::npos &&
           local_lighting.find("light.kind == LOCAL_LIGHT_SPOT") !=
               std::string::npos);
    assert(local_lighting.find("vec3 metal_f") == std::string::npos &&
           local_lighting.find(
               "mix(vec3(0.04), albedo, clamp(metallic, 0.0, 1.0))") !=
               std::string::npos);
    assert(raster_local_direct_uses_bounded_candidates(composite));
    // Fallback brute-forces every scene light.
    assert(!raster_local_direct_uses_bounded_candidates(mutated(
        composite, "? list_count : count + local_light_counts.z;",
        "? list_count : local_light_counts.x;")));
    // Fallback forgets the oversized lights.
    assert(!raster_local_direct_uses_bounded_candidates(mutated(
        composite, "? list_count : count + local_light_counts.z;",
        "? list_count : count;")));
    // Oversized lights read from the wrong offset (past the end of the list).
    assert(!raster_local_direct_uses_bounded_candidates(mutated(
        composite, "local_light_oversized_indices[candidate - count]",
        "local_light_oversized_indices[candidate]")));
    // Tile culling compiled out of the include.
    assert(!raster_local_direct_uses_bounded_candidates(mutated(
        composite, "#define MATTER_PRIMARY_LIGHT_CULLING 1", "")));
    assert(rt.find("#include \"local_lighting.glsl\"") !=
               std::string::npos &&
           rt.find("layout(set = 0, binding = 26, rgba16f)") !=
               std::string::npos &&
           rt.find("local_light_indices[offset + candidate]") !=
               std::string::npos &&
           rt.find("local_light_oversized_indices[candidate]") !=
               std::string::npos);
    assert(rt.find("distance_to_sample - constants.bias") !=
               std::string::npos &&
           rt.find("gl_RayFlagsTerminateOnFirstHitEXT |") !=
               std::string::npos &&
           rt.find("0x01, 0, 0, 0, origin") != std::string::npos &&
           rt.find("0x02, 0, 0, 0,") != std::string::npos);
    // Local-direct is dispatched separately from GI, so its dispatch bit does
    // not describe whether composite will consume the transmission lane. Pin
    // the independent scene-GI bit from the host all the way to the glass
    // fallback: using gi_dispatch here leaves GI-on glass with the GI-off
    // Fresnel-scaled diffuse weight.
    const size_t local_direct_dispatch =
        renderer_source.find("if (trace_local_direct) {");
    const size_t local_direct_dispatch_open =
        renderer_source.find('{', local_direct_dispatch);
    const size_t local_direct_dispatch_close =
        matching_closing_brace(renderer_source, local_direct_dispatch_open);
    assert(renderer_source.find(
               "constexpr uint32_t kSceneGiEnabledBit = 0x20000000u;") !=
               std::string::npos);
    assert(renderer_source.find(
               "gi_settings_.enabled ? kSceneGiEnabledBit : 0u") !=
               std::string::npos);
    assert(local_direct_dispatch != std::string::npos &&
           local_direct_dispatch_close != std::string::npos);
    const std::string local_direct_dispatch_body = renderer_source.substr(
        local_direct_dispatch,
        local_direct_dispatch_close - local_direct_dispatch + 1u);
    assert(local_direct_dispatch_body.find(
               "kLocalDirectDispatchBit | scene_gi_state") !=
           std::string::npos);
    assert(rt.find(
               "const bool scene_gi_enabled =\n"
               "        (constants.shadow_samples & 0x20000000u) != 0u;") !=
               std::string::npos);
    assert(rt.find(
               "if (!scene_gi_enabled && primary_transmission > 0.0)") !=
               std::string::npos);
    assert(rt.find("if (!gi_dispatch && primary_transmission > 0.0)") ==
           std::string::npos);

    const size_t rt_settings_setter = renderer_source.find(
        "void VkSceneRenderer::set_ray_tracing_settings(");
    const size_t rt_settings_open =
        renderer_source.find('{', rt_settings_setter);
    const size_t rt_settings_close =
        matching_closing_brace(renderer_source, rt_settings_open);
    assert(rt_settings_setter != std::string::npos &&
           rt_settings_close != std::string::npos);
    const std::string rt_settings_body = renderer_source.substr(
        rt_settings_setter, rt_settings_close - rt_settings_setter + 1u);
    assert(rt_settings_body.find("dlss_history_reset_pending_ = true;") !=
               std::string::npos &&
           rt_settings_body.find("temporal_history_changed_ = true;") !=
               std::string::npos);

    // Ground POM keeps shading/index lookup at the recessed G-buffer point,
    // but visibility rays must start at the lifted `world` position above the
    // undisplaced TLAS mesh. Secondary and hit surfaces make the same split:
    // RT surface detail may displace surface.position, so their rays start at
    // surface.visibility_position (the undisplaced proxy hit, equal to
    // position whenever no detail was applied).
    const size_t visibility_helper = rt.find(
        "vec3 local_light_visibility(LocalLightGpu light,");
    const size_t visibility_helper_open = rt.find('{', visibility_helper);
    const size_t visibility_helper_close =
        matching_closing_brace(rt, visibility_helper_open);
    assert(visibility_helper != std::string::npos &&
           visibility_helper_close != std::string::npos);
    const std::string visibility_helper_body = rt.substr(
        visibility_helper,
        visibility_helper_close - visibility_helper + 1u);
    assert(visibility_helper_body.find(
               "vec3 origin = ray_origin_position + receiver_normal * constants.bias;") !=
           std::string::npos);
    assert(visibility_helper_body.find(
               "vec3 origin = receiver_position + receiver_normal * constants.bias;") ==
           std::string::npos);
    const size_t saved_shading_world = rt.find("vec3 shading_world = world;");
    const size_t pom_lift = rt.find(
        "world += normal * (tileset.pom_b.z + 0.02);", saved_shading_world);
    const size_t primary_local_direct = rt.find(
        "vec3 primary_local_direct = evaluate_rt_local_lighting(", pom_lift);
    assert(saved_shading_world != std::string::npos &&
           pom_lift != std::string::npos &&
           primary_local_direct != std::string::npos &&
           saved_shading_world < pom_lift && pom_lift < primary_local_direct);
    assert(rt_local_lighting_splits_receiver_and_origin(rt));
    const std::string rt_surface =
        read_shader("../shaders_vk/rt_surface_common.glsl");
    assert(without_whitespace(without_comments(rt_surface))
               .find("vec3visibility_position;") != std::string::npos);
    assert(without_whitespace(without_comments(rt_surface))
               .find("surface.visibility_position=surface.position;") !=
           std::string::npos);
    // Secondary receiver and origin swapped.
    assert(!rt_local_lighting_splits_receiver_and_origin(mutated(
        rt, "surface.position, surface.visibility_position, shading_normal",
        "surface.visibility_position, surface.position, shading_normal")));
    // Hit rays start from the displaced surface again.
    assert(!rt_local_lighting_splits_receiver_and_origin(mutated(
        rt, "hit.surface.position, hit.surface.visibility_position,",
        "hit.surface.position, hit.surface.position,")));
    // Primary rays start below the lifted POM surface.
    assert(!rt_local_lighting_splits_receiver_and_origin(mutated(
        rt, "shading_world, world, direct_normal,",
        "shading_world, shading_world, direct_normal,")));
    // Primary surface detail loses its proxy geometric normal.
    assert(!rt_local_lighting_splits_receiver_and_origin(mutated(
        rt, "primary_surface_detail ? primary_geo_normal : direct_normal",
        "direct_normal")));
    // The visibility helper offsets from the receiver instead of the origin.
    assert(!rt_local_lighting_splits_receiver_and_origin(mutated(
        rt, "vec3 origin = ray_origin_position + receiver_normal * constants.bias;",
        "vec3 origin = receiver_position + receiver_normal * constants.bias;")));
    assert(composite.find("layout(set = 0, binding = 11) uniform sampler2D "
                          "local_direct_texture") != std::string::npos &&
           composite.find("local_light_counts.w == LOCAL_DIRECT_RAY_TRACED") !=
               std::string::npos &&
           composite.find("texture(local_direct_texture, in_uv).rgb") !=
               std::string::npos);
    assert(composite.find("raw_diffuse + local_direct.diffuse") ==
               std::string::npos &&
           composite.find("specular + local_direct.specular") ==
               std::string::npos);
    assert(composite.find("environment.direct_world_sun_ratio.rgb") !=
               std::string::npos &&
           composite.find("environment.sun_disc_reserved.rgb") !=
               std::string::npos);
    assert(rt.find("environment.direct_world_sun_ratio.rgb") !=
               std::string::npos &&
           rt.find("environment.sun_disc_reserved.rgb") !=
               std::string::npos &&
           rt.find("environment.sky_display_reserved.rgb") !=
               std::string::npos);
    assert(volume.find("environment.direct_world_sun_ratio.rgb") !=
               std::string::npos &&
           volume.find("sample_sky_irradiance") != std::string::npos);
    // Task 12: one superset scatter shader specializes the enhanced cloud
    // path away for Current cost, reads the R16F detail grid only when live,
    // and consumes the post-adjustment environment lighting ABI.
    for (const char* required : {
             "layout(constant_id = 0) const bool ENHANCED_CLOUD_LIGHTING",
             "uniform sampler3D vol_cloud_density",
             "world_to_froxel_uvw",
             "tau_local_full + tau_remaining_coarse"})
        assert(volume.find(required) != std::string::npos);
    const std::string volume_common =
        read_shader("../shaders_vk/vol_common.glsl");
    assert(volume_common.find(
               "0.8 * hg_phase(mu, 0.85 * anisotropy_scale)") !=
               std::string::npos &&
           volume_common.find(
               "0.2 * hg_phase(mu, -0.30 * anisotropy_scale)") !=
               std::string::npos);
    // Terrain occlusion for cloud-bearing froxels extends the existing ray;
    // it must not silently add a second per-froxel query or lengthen the
    // fog-only/Current path.  Ordering pins the density decision before the
    // one real ray and its sanitized value before cloud scattering consumes it.
    const size_t cloud_sample = volume.find(
        "cloud_extinction = texture(vol_cloud_density, uvw).r");
    const size_t ray_query = volume.find("rayQueryInitializeEXT");
    const size_t cloud_scattering = volume.find(
        "vec3 cloud_scattering = vec3(0.99) * cloud_extinction");
    assert(volume_common.find(
               "const float VOL_CLOUD_TERRAIN_SHADOW_FAR = VOL_FROXEL_FAR") !=
               std::string::npos);
    assert(volume.find("cloud_extinction > 1e-6") != std::string::npos);
    assert(volume.find("VOL_CLOUD_TERRAIN_SHADOW_FAR") != std::string::npos);
    assert(count_occurrences(volume, "rayQueryInitializeEXT") == 1);
    assert(cloud_sample != std::string::npos && ray_query != std::string::npos &&
           cloud_scattering != std::string::npos && cloud_sample < ray_query &&
           ray_query < cloud_scattering);
    assert(volume.find("pc.sun_color") == std::string::npos &&
           volume.find("pc.sky_color") == std::string::npos &&
           volume.find("sun_intensity") == std::string::npos);
    const std::string volumetrics_header =
        read_shader("../src/render/vk_volumetrics.h");
    assert(volumetrics_header.find(
               "static_assert(sizeof(ScatterConstants) == 240)") !=
               std::string::npos);
    for (const char* offset : {"offsetof(ScatterConstants, camera_pos) == 128",
                               "offsetof(ScatterConstants, local_sun_march_steps) == 156",
                               "offsetof(ScatterConstants, camera_fwd) == 192",
                               "offsetof(ScatterConstants, local_march_distance_m) == 236"})
        assert(volumetrics_header.find(offset) != std::string::npos);
    assert(volumetrics_header.find("sun_intensity") == std::string::npos);
    const std::string volumetrics_host =
        read_shader("../src/render/vk_volumetrics.cpp");
    assert(volumetrics_host.find(
               "volumetric_camera_eye(matrices.world_to_view)") !=
           std::string::npos);
    assert(volume.find("view_depth + 1.0e-5 < pc.camera_near") !=
           std::string::npos);
    const std::string sky_view =
        read_shader("../shaders_vk/atmosphere_sky_view.comp");
    assert(sky_view.find("(float(pixel.x) + 0.5) / 192.0") !=
           std::string::npos);
    assert(sky_view.find("float(pixel.x) / 191.0") == std::string::npos);
    const std::string display =
        read_shader("../shaders_vk/display_transform.frag");
    const std::string ranks =
        "37,12,54,1,46,27,61,8,18,43,5,58,31,50,14,40,"
        "63,22,35,10,48,3,56,29,16,45,7,60,25,52,11,38,"
        "33,0,47,20,57,15,42,30,9,53,24,62,4,36,19,51,"
        "41,13,55,28,59,6,44,21,26,49,2,39,17,34,23,32";
    assert(display.find(ranks) != std::string::npos);
    for (const char* required : {"gl_FragCoord", "linear_to_srgb",
                                 "srgb_to_linear", "code_dithered"})
        assert(display.find(required) != std::string::npos);
    for (const char* forbidden : {"frame_index", "jitter", "time",
                                  "random", "pcg"})
        assert(display.find(forbidden) == std::string::npos);
    const std::string renderer = read_shader("../src/render/vk_scene_renderer.cpp");
    assert(renderer.find("VK_FORMAT_R16_SFLOAT") != std::string::npos &&
           renderer.find("record_neutral_cloud_clear") != std::string::npos &&
           renderer.find("update_environment_descriptor(selected, error)") !=
               std::string::npos &&
           renderer.find("volumetrics_->invalidate_history()") !=
               std::string::npos);
    const std::string atmosphere_host = read_shader("../src/render/vk_atmosphere.cpp");
    assert(!atmosphere_host.empty());
    // Review regressions: sky-view must be visible to every Task 7 consumer,
    // and the environment UBO must be mapped before its neutral std140 bytes
    // are initialized. These source contracts complement the real raster
    // readback because neither property is uniquely observable in one pixel.
    //
    // 2026-08-20: ray tracing is an OPTIONAL device feature and
    // VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR is ILLEGAL in a barrier
    // stage mask on a device that did not enable it
    // (VUID-VkImageMemoryBarrier2-dstStageMask-07946, ten of them per frame in
    // the vt-enrich-nort smoke mode). The "readable by every shader stage"
    // transitions therefore fold the stage in through
    // matter::ray_tracing_shader_stage(ray_tracing_available()), which yields 0
    // on an RT-less device -- so the bit is no longer named literally here and
    // these assertions pin the SAME property through that helper instead. What
    // must still hold: the RT stage is derived from the live device query, and
    // the sky-view SHADER_READ_ONLY transition's DESTINATION stage mask still
    // carries it alongside COMPUTE and FRAGMENT. Dropping ray tracing from that
    // mask, or hard-wiring the helper's argument, still fails.
    assert(atmosphere_host.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") ==
           std::string::npos);
    const size_t rt_stage_binding =
        atmosphere_host.find("VkPipelineStageFlags2 ray_tracing_stage =");
    assert(rt_stage_binding != std::string::npos);
    const std::string rt_stage_tail = atmosphere_host.substr(rt_stage_binding, 200);
    assert(rt_stage_tail.find("matter::ray_tracing_shader_stage(") !=
               std::string::npos &&
           rt_stage_tail.find("ray_tracing_available()") != std::string::npos);
    const size_t sky_transition = atmosphere_host.find(
        "record_image_transition(command_buffer, sky_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL");
    assert(sky_transition != std::string::npos &&
           rt_stage_binding < sky_transition);
    const std::string sky_tail = atmosphere_host.substr(sky_transition, 520);
    const size_t sky_destination_mask = sky_tail.find(
        "VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |");
    assert(sky_destination_mask != std::string::npos);
    // The gated ray-tracing stage has to be OR'd into that same destination
    // mask, i.e. before the destination access mask closes the argument list.
    const size_t sky_ray_tracing =
        sky_tail.find("ray_tracing_stage", sky_destination_mask);
    const size_t sky_destination_access =
        sky_tail.find("VK_ACCESS_2_SHADER_SAMPLED_READ_BIT", sky_destination_mask);
    assert(sky_ray_tracing != std::string::npos &&
           sky_destination_access != std::string::npos &&
           sky_ray_tracing < sky_destination_access);
    // The same gating covers the cloud-shadow volumes, which are transitioned
    // "readable by every shader stage" for the same reason.
    const std::string cloud_shadow_host =
        read_shader("../src/render/vk_cloud_shadows.cpp");
    assert(!cloud_shadow_host.empty());
    assert(cloud_shadow_host.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") ==
               std::string::npos &&
           cloud_shadow_host.find("matter::ray_tracing_shader_stage(") !=
               std::string::npos &&
           cloud_shadow_host.find("ray_tracing_available()") != std::string::npos);
    // The helper itself must stay conditional: a version that always returns
    // the bit would re-introduce the very validation error it exists to avoid.
    const std::string vk_resources_header =
        read_shader("../src/render/vk_resources.h");
    const size_t rt_stage_helper =
        vk_resources_header.find("ray_tracing_shader_stage(");
    assert(rt_stage_helper != std::string::npos);
    const std::string rt_stage_helper_tail =
        vk_resources_header.substr(rt_stage_helper, 260);
    assert(rt_stage_helper_tail.find("ray_tracing_available") != std::string::npos &&
           rt_stage_helper_tail.find("VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR") !=
               std::string::npos &&
           rt_stage_helper_tail.find("VkPipelineStageFlags2{0}") != std::string::npos);
    assert(renderer.find("matter::map_buffer(frame.environment_constants, error)") !=
           std::string::npos &&
           renderer.find("environment_constants.mapped == nullptr") ==
               std::string::npos);
    const size_t neutral_environment_block = renderer.find("float block[56]{};");
    assert(neutral_environment_block != std::string::npos);
    const std::string neutral_environment_tail =
        renderer.substr(neutral_environment_block, 3200);
    assert(neutral_environment_tail.find("block[0] = block[5] = block[10] = block[15] = 1.0f;") !=
               std::string::npos &&
           neutral_environment_tail.find("block[16] = block[21] = block[26] = block[31] = 1.0f;") !=
               std::string::npos &&
           neutral_environment_tail.find("std::memcpy(frame.environment_constants.mapped") !=
               std::string::npos &&
           neutral_environment_tail.find("matter::flush_buffer(frame.environment_constants,") !=
               std::string::npos);
    const std::string engine_make = read_shader("../Makefile");
    const std::string editor_make = read_shader("../../MatterEditor/Makefile");
    assert(engine_make.find("build/shaders_vk/vol_scatter.comp.spv: shaders_vk/environment_common.glsl") !=
           std::string::npos);
    // Every shader whose contract this test states must also be rebuilt by
    // the Make build when any file in its include closure changes; otherwise
    // the embedded SPIR-V silently keeps the old contract. This is checked
    // against the parsed rules rather than one rule's exact spelling, so
    // regrouping targets stays legal while a missing edge does not.
    std::vector<std::string> spirv_entries = {
        "shaders_vk/composite.frag",     "shaders_vk/vol_scatter.comp",
        "shaders_vk/water_forward.frag", "shaders_vk/gbuffer.frag",
        "shaders_vk/raster.vert",        "shaders_vk/cull.comp",
        "shaders_vk/geometry_select.comp"};
    spirv_entries.insert(spirv_entries.end(), rt_lighting_entries.begin(),
                         rt_lighting_entries.end());
    // First missing edge, or empty when every closure is covered.
    auto spirv_rule_gap = [&](const std::string& makefile) {
        const auto rules = spirv_prerequisites(makefile);
        std::string missing;
        for (const std::string& entry : spirv_entries)
            if (!spirv_rule_covers_includes(rules, entry, missing)) break;
        return missing;
    };
    auto spirv_rules_cover = [&](const std::string& makefile) {
        return spirv_rule_gap(makefile).empty();
    };
    const std::string make_gap = spirv_rule_gap(engine_make);
    if (!make_gap.empty())
        fprintf(stderr, "shader_source_tests: %s\n", make_gap.c_str());
    assert(make_gap.empty());
    // local_lighting.glsl dropped from composite/rt_lighting/rt_primary_adaptive.
    assert(!spirv_rules_cover(mutated(
        engine_make,
        "    shaders_vk/local_lighting.glsl shaders_vk/local_light_types.glsl",
        "    shaders_vk/local_light_types.glsl")));
    // Shared RT implementation edge dropped from the two primary wrappers.
    assert(!spirv_rules_cover(mutated(
        engine_make,
        "build/shaders_vk/rt_primary_adaptive.rgen.spv "
        "build/shaders_vk/rt_lighting.rgen.spv: shaders_vk/rt_lighting_impl.glsl",
        "")));
    // Culling/audit wrappers narrowed to the implementation file alone.
    assert(!spirv_rules_cover(mutated(engine_make, "    $(wildcard shaders_vk/*.glsl)",
                                      "    shaders_vk/rt_lighting_impl.glsl")));
    // An order-only prerequisite never triggers a rebuild.
    assert(!spirv_rules_cover(mutated(
        engine_make, "build/shaders_vk/cull.comp.spv: shaders_vk/scene_instance.glsl",
        "build/shaders_vk/cull.comp.spv: | shaders_vk/scene_instance.glsl")));
    // A commented-out rule is no rule.
    assert(!spirv_rules_cover(mutated(
        engine_make, "build/shaders_vk/cull.comp.spv: shaders_vk/scene_instance.glsl",
        "# build/shaders_vk/cull.comp.spv: shaders_vk/scene_instance.glsl")));
    // 2026-08-14: MatterEditor/Makefile used to keep a second, independently
    // maintained copy of the VK_SPV list and every .glsl dependency edge,
    // regenerating the SAME shaders_gen/embedded_spirv.h -- the two drifted,
    // and this test used to guard the drift directly by asserting both
    // copies had this one edge. MatterEngine3/Makefile is now the single
    // source of truth and MatterEditor delegates to its `vulkan-spirv`
    // target instead, so the guard now is that the delegation exists and
    // the old duplicate edge is gone (a duplicate reappearing here is
    // exactly the drift hazard this test exists to catch).
    assert(editor_make.find("embedded-spirv") != std::string::npos &&
           editor_make.find("$(MAKE) -C $(ME3_DIR) vulkan-spirv") != std::string::npos);
    assert(editor_make.find("build/shaders_vk/vol_scatter.comp.spv: $(ME3_DIR)/shaders_vk/environment_common.glsl") ==
           std::string::npos);
    printf("shader_source_tests: all passed\n");
    return 0;
}
