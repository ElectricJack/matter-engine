#include "legacy_part_route.h"

#include <matter/authoring_adapter/pipeline.h>
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "part_asset_v2.h"
#include "part_base.js.h"
#include "part_render_policy.h"
#include "script_host.h"
#include "matter/log.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <regex>
#include <set>
#include <unordered_map>
#include <variant>

namespace matter::authoring_adapter {
namespace {
namespace d = matter::contracts;
namespace s = matter::services;

// Versions this conversion (entry synthesis + Tri/TriEx emission). Bump it when
// either changes what a portable part bakes to.
constexpr const char* kRouteVersion = "matter.engine.legacy-part-route.v1";
constexpr const char* kMaterialPrefix = "matter.legacy.material.";

std::string join_diagnostics(const std::vector<d::Diagnostic>& diagnostics) {
    std::string out;
    for (const auto& x : diagnostics)
        out += std::string(d::code_name(x.code)) + ": " + x.message + "; ";
    return out;
}

// The legacy vocabulary is read from kPartBaseJS itself so the portable shim can
// never drift from it: the MAT table verbatim, and every Part method except
// build() as the set of verbs delegated to the SDK's legacyPart adapter (which
// rejects the ones outside its portable subset with unsupported_capability).
struct LegacyVocabulary { std::string mat, verbs; };
const LegacyVocabulary& legacy_vocabulary() {
    static const LegacyVocabulary vocabulary = [] {
        LegacyVocabulary v;
        const std::string base = kPartBaseJS;
        const auto mat = base.find("globalThis.MAT = ");
        const auto mat_end = base.find("\n};", mat);
        if (mat != std::string::npos && mat_end != std::string::npos)
            v.mat = base.substr(mat + 17, mat_end + 2 - (mat + 17));
        const auto part = base.find("globalThis.Part = class Part {");
        const auto part_end = base.find("\n};", part);
        if (part != std::string::npos && part_end != std::string::npos) {
            static const std::regex method(R"(\n  ([A-Za-z_$][A-Za-z0-9_$]*)\s*\()");
            const std::string body = base.substr(part, part_end - part);
            for (std::sregex_iterator it(body.begin(), body.end(), method), end; it != end; ++it)
                if ((*it)[1] != "build") v.verbs += (v.verbs.empty() ? "'" : ",'") + (*it)[1].str() + "'";
        }
        return v;
    }();
    return vocabulary;
}

// One module per source: the legacy class body runs verbatim against a Part
// shim whose verbs forward to sdk.legacyPart; numeric MAT ids become declared
// placeholder materials the bake maps back to the engine palette.
std::string entry_text(const std::string& source, const std::string& class_name) {
    const auto& vocabulary = legacy_vocabulary();
    return "import * as sdk from 'matter:authoring';\n"
           "const MAT = Object.freeze(" + vocabulary.mat + ");\n"
           "const LEGACY_VERBS = new Set([" + vocabulary.verbs + "]);\n"
           "const LEGACY = Symbol('legacy');\n"
           "class Part { constructor() { return new Proxy(this, { get(t, k, r) {\n"
           "  if (typeof k === 'string' && !(k in t) && LEGACY_VERBS.has(k)) return (...a) => t[LEGACY][k](...a);\n"
           "  return Reflect.get(t, k, r); } }); } }\n" +
           source + "\n;const __partClass = " + class_name + ";\n"
           "export default (ctx) => {\n"
           "  for (const key of Object.getOwnPropertyNames(__partClass))\n"
           "    if (!['length', 'name', 'prototype', 'params', 'authoring'].includes(key))\n"
           "      throw new sdk.AuthoringError('unsupported_capability', `legacy static ${key} has no portable adapter`);\n"
           "  const supplied = Object.assign({}, __partClass.params ?? {}, ctx.parameters);\n"
           "  const merged = {};\n"
           "  for (const key of Object.keys(supplied).sort()) merged[key] = supplied[key];\n"
           "  const b = new sdk.RecipeBuilder({ ...ctx, parameters: merged });\n"
           "  const declared = new Set();\n"
           "  const materialIds = new Proxy({}, { get(_, k) {\n"
           "    if (typeof k !== 'string' || !/^[0-9]+$/.test(k)) return undefined;\n"
           "    const id = '" + kMaterialPrefix + "' + k;\n"
           "    if (!declared.has(id)) { declared.add(id); b.material(id, {}); }\n"
           "    return id; } });\n"
           "  const part = new __partClass();\n"
           "  part[LEGACY] = sdk.legacyPart(b, { prefix: 'part', materialIds });\n"
           "  part.build(merged);\n"
           "  return part[LEGACY].finish();\n"
           "};\n";
}

float3 to_float3(const d::Vec3& v) { return make_float3(v.x, v.y, v.z); }

// Built products by part hash, shared by every route instance: the hash is
// content-addressed, so one product serves all of them and is never replaced.
struct RetainedProducts {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<const s::AssetProduct>> by_hash;
};
RetainedProducts& retained_products() {
    static RetainedProducts products;
    return products;
}

class LegacyPartRoute final : public part_graph::PortableBakeRoute {
public:
    uint64_t resolve(const std::string& source, const std::string& params_json,
                     std::string* merged_params_out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        s::BuildRequest request;
        std::shared_ptr<const d::RecipeDocument> recipe;
        const uint64_t hash = author(source, params_json, request, recipe, merged_params_out);
        // Retain the product now: a warm part cache never reaches bake().
        // Failures are bake()'s to report, so resolution is unchanged.
        if (hash) build_retained(hash, request, false);
        return hash;
    }

    bool bake(const std::string& source, const std::string& params_json,
              uint64_t resolved_hash, const std::string& parts_dir) override {
        std::lock_guard<std::mutex> lock(mutex_);
        s::BuildRequest request;
        std::shared_ptr<const d::RecipeDocument> recipe;
        const uint64_t hash = author(source, params_json, request, recipe, nullptr);
        if (!hash) return false;
        if (hash != resolved_hash) {
            MATTER_LOGE("portable", "bake: resolved hash %016llx no longer matches %016llx",
                        (unsigned long long)resolved_hash, (unsigned long long)hash);
            return false;
        }
        const auto product = build_retained(hash, request, true);
        if (!product) return false;
        std::vector<Tri> tris;
        std::vector<TriEx> extra;
        if (!emit_triangles(product->document, tris, extra)) return false;

        BLASManager blas;
        TLASManager tlas(65536);
        script_host::register_direct_triangles(blas, tlas, tris, extra);
        tlas.build(blas);
        const std::string rel = part_asset::cache_path_resolved(hash);
        const std::string path = parts_dir.empty() ? rel : parts_dir + "/" + rel;
        if (!part_asset::save_v2_with_render_policy(path, blas, tlas, nullptr, 0,
                                                    part_asset::LodLevels{}, {}, hash,
                                                    matter::PartRenderPolicy{}, nullptr)) {
            MATTER_LOGE("portable", "bake: failed to publish %s", path.c_str());
            return false;
        }
        return true;
    }

private:
    // Builds an authored request once per hash and retains the product, so the
    // .part and the graphics adapter consume the same immutable document.
    std::shared_ptr<const s::AssetProduct> build_retained(uint64_t hash, const s::BuildRequest& request,
                                                          bool report) {
        if (auto kept = retained_asset(hash)) return kept;
        const s::JobEnvelope job{"matter.engine.part", "1", ++epoch_};
        if (!pipeline_.coordinator.activate(job).ok()) {
            if (report) MATTER_LOGE("portable", "bake: coordinator refused activation");
            return nullptr;
        }
        const auto built = pipeline_.coordinator.build(job, request, {});
        if (!built.ok()) {
            if (report) MATTER_LOGE("portable", "bake: %s", join_diagnostics(built.diagnostics()).c_str());
            return nullptr;
        }
        auto& products = retained_products();
        std::lock_guard<std::mutex> lock(products.mutex);
        return products.by_hash.emplace(hash, built.value()).first->second;
    }

    // Freezes the synthesized entry and authors it. The hash namespaces the
    // canonical recipe digest with the pipeline implementation and this route's
    // version, so it can never alias a legacy ScriptHost hash. 0 on failure.
    uint64_t author(const std::string& source, const std::string& params_json,
                    s::BuildRequest& request, std::shared_ptr<const d::RecipeDocument>& recipe,
                    std::string* merged_params_out) {
        const std::string class_name = script_host::find_part_class_name(source);
        if (class_name.empty()) {
            MATTER_LOGE("portable", "resolve: no class extending Part found");
            return 0;
        }
        d::Value parameters;
        if (!d::parse_json(params_json.empty() ? "{}" : params_json, parameters).ok() ||
            !std::holds_alternative<d::Value::Object>(parameters.data)) {
            MATTER_LOGE("portable", "resolve: parameters are not a JSON object");
            return 0;
        }
        const std::string text = entry_text(source, class_name);
        const std::string digest = d::sha256(text).hex;
        const std::string entry = "matter-engine/part-" + digest.substr(0, 16) + ".mjs";
        d::SourceIdentity identity{{entry}, "matter-part:sha256/" + digest, "1", {}, {}};
        pipeline_.resolver.freeze(s::SourceSnapshot{{entry}, {digest}, {{identity, text, {}}}, {}});
        request.source.source = identity;
        request.authoring.parameters = std::move(parameters);

        const s::JobEnvelope job{"matter.engine.part", "1", ++epoch_};
        const auto snapshot = pipeline_.resolver.resolve(job, request.source, {}, {});
        const auto authored = snapshot.ok()
            ? pipeline_.host->author(job, *snapshot.value(), request.authoring, {}, {})
            : s::RecipeResult::failure(job, snapshot.diagnostics());
        if (!authored.ok()) {
            MATTER_LOGE("portable", "resolve %s: %s", class_name.c_str(),
                        join_diagnostics(authored.diagnostics()).c_str());
            return 0;
        }
        recipe = authored.value();
        d::ContentDigest recipe_digest;
        std::string merged;
        if (!d::content_digest(*recipe, recipe_digest).ok() ||
            !d::canonical_json(recipe->parameters, merged).ok()) {
            MATTER_LOGE("portable", "resolve %s: recipe failed canonicalization", class_name.c_str());
            return 0;
        }
        const std::string keyed = d::sha256(AuthoringPipeline::options().implementation.hex + "|" +
                                            kRouteVersion + "|" + recipe_digest.hex).hex;
        uint64_t hash = std::stoull(keyed.substr(0, 16), nullptr, 16);
        if (!hash) hash = 1;
        if (merged_params_out) *merged_params_out = merged;
        return hash;
    }

    // Flattens the asset's mesh instances into the engine's Tri/TriEx stream.
    // Parent chains compose parent * local (as SceneBackend::expand does);
    // bake_placement owns normal and mirrored-winding correction.
    static bool emit_triangles(const d::AssetDocument& asset, std::vector<Tri>& tris,
                               std::vector<TriEx>& extra) {
        for (const auto& instance : asset.instances) {
            if (instance.target != d::InstanceTarget::mesh) {
                MATTER_LOGE("portable", "bake: assembly instance %s needs the child-part adapter",
                            instance.id.value.c_str());
                return false;
            }
            mm::Mat4 world;
            if (!compose_instance_transform(asset, instance, world)) return false;
            const d::MeshPrototype* mesh = nullptr;
            for (const auto& candidate : asset.meshes)
                if (candidate.id.value == instance.prototype.value) mesh = &candidate;
            d::MeshPrototype placed;
            if (!mesh || !geometry::bake_placement(*mesh, world, placed).ok()) {
                MATTER_LOGE("portable", "bake: cannot place mesh %s", instance.prototype.value.c_str());
                return false;
            }
            const bool normals = placed.normals.size() == placed.positions.size();
            const bool uvs = !placed.uv_sets.empty() && placed.uv_sets[0].size() == placed.positions.size();
            for (const auto& submesh : placed.submeshes) {
                const std::string& material = submesh.material.value;
                if (material.compare(0, std::strlen(kMaterialPrefix), kMaterialPrefix) != 0 ||
                    material.size() == std::strlen(kMaterialPrefix)) {
                    MATTER_LOGE("portable", "bake: material '%s' has no engine palette id", material.c_str());
                    return false;
                }
                const int material_id = std::stoi(material.substr(std::strlen(kMaterialPrefix)));
                for (uint32_t i = submesh.first_index; i + 2 < submesh.first_index + submesh.index_count; i += 3) {
                    const uint32_t ix[3] = {placed.indices[i], placed.indices[i + 1], placed.indices[i + 2]};
                    Tri t{};
                    t.vertex0 = to_float3(placed.positions[ix[0]]);
                    t.vertex1 = to_float3(placed.positions[ix[1]]);
                    t.vertex2 = to_float3(placed.positions[ix[2]]);
                    t.centroid = (t.vertex0 + t.vertex1 + t.vertex2) * (1.0f / 3.0f);
                    TriEx e{};
                    const float3 face = normalize(cross(t.vertex1 - t.vertex0, t.vertex2 - t.vertex0));
                    e.N0 = normals ? to_float3(placed.normals[ix[0]]) : face;
                    e.N1 = normals ? to_float3(placed.normals[ix[1]]) : face;
                    e.N2 = normals ? to_float3(placed.normals[ix[2]]) : face;
                    if (uvs) {
                        const auto& uv = placed.uv_sets[0];
                        e.uv0 = make_float2(uv[ix[0]].x, uv[ix[0]].y);
                        e.uv1 = make_float2(uv[ix[1]].x, uv[ix[1]].y);
                        e.uv2 = make_float2(uv[ix[2]].x, uv[ix[2]].y);
                    }
                    e.materialId = material_id;
                    e.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                    tris.push_back(t);
                    extra.push_back(e);
                }
            }
        }
        if (tris.empty()) {
            MATTER_LOGE("portable", "bake: portable part produced no triangles");
            return false;
        }
        return true;
    }

    std::mutex mutex_;
    uint64_t epoch_ = 0;
    AuthoringPipeline pipeline_;
};
} // namespace

std::shared_ptr<part_graph::PortableBakeRoute> make_legacy_part_route() {
    return std::make_shared<LegacyPartRoute>();
}

std::shared_ptr<const s::AssetProduct> retained_asset(uint64_t part_hash) {
    auto& products = retained_products();
    std::lock_guard<std::mutex> lock(products.mutex);
    const auto found = products.by_hash.find(part_hash);
    return found == products.by_hash.end() ? nullptr : found->second;
}

bool compose_instance_transform(const d::AssetDocument& asset, const d::Instance& instance,
                                mm::Mat4& world) {
    world = instance.local_transform;
    const d::Instance* link = &instance;
    for (size_t depth = 0; link->parent; ++depth) {
        const d::Instance* parent = nullptr;
        for (const auto& other : asset.instances)
            if (other.id.value == link->parent->value) parent = &other;
        if (!parent || depth > asset.instances.size()) {
            MATTER_LOGE("portable", "bake: broken parent chain at %s", link->id.value.c_str());
            return false;
        }
        world = mm::multiply(parent->local_transform, world);
        link = parent;
    }
    return true;
}
} // namespace matter::authoring_adapter
