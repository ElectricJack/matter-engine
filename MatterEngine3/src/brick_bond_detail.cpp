#include "brick_bond_detail.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#if defined(MATTER_HAVE_SCRIPT_HOST)
extern "C" {
#include "quickjs.h"
}
#endif
namespace detail_bake {
#if defined(MATTER_HAVE_SCRIPT_HOST)
namespace {
struct Context {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = rt ? JS_NewContext(rt) : nullptr;
    ~Context() {
        if (ctx)
            JS_FreeContext(ctx);
        if (rt)
            JS_FreeRuntime(rt);
    }
};
struct Value {
    JSContext *ctx;
    JSValue value;
    ~Value() { JS_FreeValue(ctx, value); }
};
bool fail(std::string &e, const std::string &message) {
    e = "brickBondV1: " + message;
    return false;
}
bool object(JSValueConst v) { return JS_IsObject(v) && !JS_IsArray(v); }
bool keys(JSContext *ctx, JSValueConst value, std::initializer_list<const char *> allowed,
          std::string &e) {
    JSPropertyEnum *names = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) <
        0)
        return fail(e, "cannot enumerate descriptor");
    bool okay = true;
    for (uint32_t i = 0; i < count; ++i) {
        const char *text = JS_AtomToCString(ctx, names[i].atom);
        bool known = false;
        for (const char *candidate : allowed)
            if (text && std::strcmp(text, candidate) == 0)
                known = true;
        if (!known && okay) {
            fail(e, std::string("unknown property ") + (text ? text : "?"));
            okay = false;
        }
        if (text)
            JS_FreeCString(ctx, text);
        JS_FreeAtom(ctx, names[i].atom);
    }
    js_free(ctx, names);
    return okay;
}
bool number(JSContext *ctx, JSValueConst parent, const char *key, double lo, double hi,
            double &value, std::string &e, bool integer = false) {
    Value v{ctx, JS_GetPropertyStr(ctx, parent, key)};
    if (JS_IsUndefined(v.value))
        return true;
    double n = 0;
    if (!JS_IsNumber(v.value) || JS_ToFloat64(ctx, &n, v.value) < 0 || !std::isfinite(n) ||
        n < lo || n > hi || (integer && std::floor(n) != n))
        return fail(e, std::string("invalid ") + key);
    value = n;
    return true;
}
bool rgb(JSContext *ctx, JSValueConst value, std::array<uint8_t, 3> &out, std::string &e) {
    if (!JS_IsArray(value))
        return fail(e, "palette entry must be RGB bytes");
    Value length{ctx, JS_GetPropertyStr(ctx, value, "length")};
    uint32_t count = 0;
    JS_ToUint32(ctx, &count, length.value);
    if (count != 3)
        return fail(e, "palette entry must contain3 bytes");
    for (uint32_t i = 0; i < 3; ++i) {
        Value v{ctx, JS_GetPropertyUint32(ctx, value, i)};
        double n = 0;
        if (!JS_IsNumber(v.value) || JS_ToFloat64(ctx, &n, v.value) < 0 || !std::isfinite(n) ||
            n < 0 || n > 255 || std::floor(n) != n)
            return fail(e, "invalid RGB byte");
        out[i] = uint8_t(n);
    }
    return true;
}
} // namespace
#endif
bool parse_brick_bond_descriptor(const std::string &json, BrickBondDescriptor &out,
                                 bool &recognized, std::string &e) {
    recognized = false;
    e.clear();
#if !defined(MATTER_HAVE_SCRIPT_HOST)
    (void)json;
    (void)out;
    e = "brick bond descriptors require ScriptHost";
    return false;
#else
    if (json.size() > 65536)
        return fail(e, "descriptor exceeds64KiB");
    Context c;
    if (!c.ctx)
        return fail(e, "JSON context allocation failed");
    Value root{c.ctx, JS_ParseJSON(c.ctx, json.c_str(), json.size(), "<detail-descriptor>")};
    if (JS_IsException(root.value) || !object(root.value))
        return fail(e, "invalid merged params JSON");
    Value kind{c.ctx, JS_GetPropertyStr(c.ctx, root.value, "detailBake")};
    if (JS_IsUndefined(kind.value))
        return true;
    recognized = true;
    const char *text = JS_IsString(kind.value) ? JS_ToCString(c.ctx, kind.value) : nullptr;
    bool valid = text && std::strcmp(text, "brickBondV1") == 0;
    if (text)
        JS_FreeCString(c.ctx, text);
    if (!valid)
        return fail(e, "unsupported detailBake kind");
    if (!keys(c.ctx, root.value,
              {"detailBake", "sourceModule", "sourceParams", "variants", "bond", "projection"}, e))
        return false;
    BrickBondDescriptor result;
    Value module{c.ctx, JS_GetPropertyStr(c.ctx, root.value, "sourceModule")};
    text = JS_IsString(module.value) ? JS_ToCString(c.ctx, module.value) : nullptr;
    if (text) {
        result.source_module = text;
        JS_FreeCString(c.ctx, text);
    }
    if (result.source_module.empty() || result.source_module.size() > 128 ||
        !std::all_of(result.source_module.begin(), result.source_module.end(),
                     [](unsigned char ch) {
                         return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
                     }))
        return fail(e, "sourceModule must name one object-root module");
    Value params{c.ctx, JS_GetPropertyStr(c.ctx, root.value, "sourceParams")};
    if (!JS_IsUndefined(params.value)) {
        if (!object(params.value))
            return fail(e, "sourceParams must be an object");
        Value encoded{c.ctx, JS_JSONStringify(c.ctx, params.value, JS_UNDEFINED, JS_UNDEFINED)};
        text = JS_ToCString(c.ctx, encoded.value);
        if (!text)
            return fail(e, "sourceParams serialization failed");
        result.source_params_json = text;
        JS_FreeCString(c.ctx, text);
    }
    double variants = 8;
    if (!number(c.ctx, root.value, "variants", 8, 8, variants, e, true))
        return false;
    Value bond{c.ctx, JS_GetPropertyStr(c.ctx, root.value, "bond")};
    if (!JS_IsUndefined(bond.value)) {
        if (!object(bond.value) ||
            !keys(c.ctx, bond.value,
                  {"brickWidthM", "brickHeightM", "pitchUM", "pitchVM", "columns", "rows",
                   "tilePixels", "seed", "nominalFaceHeightM", "mortarHeightM", "brickRgb",
                   "mortarRgb", "brickRoughness", "mortarRoughness"},
                  e))
            return fail(e, e.empty() ? "bond must be an object" : e);
        auto scalar = [&](const char *key, float &v, double lo, double hi) {
            double n = v;
            if (!number(c.ctx, bond.value, key, lo, hi, n, e))
                return false;
            v = float(n);
            return true;
        };
        auto integer = [&](const char *key, uint32_t &v, double lo, double hi) {
            double n = v;
            if (!number(c.ctx, bond.value, key, lo, hi, n, e, true))
                return false;
            v = uint32_t(n);
            return true;
        };
        auto &b = result.bond;
        if (!scalar("brickWidthM", b.brick_width_m, .04, 2) ||
            !scalar("brickHeightM", b.brick_height_m, .04, 2) ||
            !scalar("pitchUM", b.pitch_u_m, .04, 2) || !scalar("pitchVM", b.pitch_v_m, .04, 2) ||
            !scalar("nominalFaceHeightM", b.nominal_face_height_m, .02, 1) ||
            !scalar("mortarHeightM", b.mortar_height_m, -.1, 0) ||
            !integer("columns", b.columns, 4, 4) || !integer("rows", b.rows, 8, 8) ||
            !integer("tilePixels", b.tile_pixels, 512, 512) ||
            !integer("seed", b.seed, 0, 4294967295.0))
            return false;
        if (b.brick_width_m >= b.pitch_u_m || b.brick_height_m >= b.pitch_v_m ||
            std::abs(b.pitch_u_m * b.columns - b.pitch_v_m * b.rows) > 1e-5f)
            return fail(e, "bond pitches require positive mortar and square periodic domain");
        Value palette{c.ctx, JS_GetPropertyStr(c.ctx, bond.value, "brickRgb")};
        if (!JS_IsUndefined(palette.value)) {
            if (!JS_IsArray(palette.value))
                return fail(e, "brickRgb must have8 RGB entries");
            Value length{c.ctx, JS_GetPropertyStr(c.ctx, palette.value, "length")};
            uint32_t n = 0;
            JS_ToUint32(c.ctx, &n, length.value);
            if (n != 8)
                return fail(e, "brickRgb must have8 RGB entries");
            for (uint32_t i = 0; i < 8; ++i) {
                Value v{c.ctx, JS_GetPropertyUint32(c.ctx, palette.value, i)};
                if (!rgb(c.ctx, v.value, b.brick_rgb[i], e))
                    return false;
            }
        }
        Value mortar{c.ctx, JS_GetPropertyStr(c.ctx, bond.value, "mortarRgb")};
        if (!JS_IsUndefined(mortar.value) && !rgb(c.ctx, mortar.value, b.mortar_rgb, e))
            return false;
        double rough = b.brick_roughness;
        if (!number(c.ctx, bond.value, "brickRoughness", 0, 255, rough, e, true))
            return false;
        b.brick_roughness = uint8_t(rough);
        rough = b.mortar_roughness;
        if (!number(c.ctx, bond.value, "mortarRoughness", 0, 255, rough, e, true))
            return false;
        b.mortar_roughness = uint8_t(rough);
    }
    Value projection{c.ctx, JS_GetPropertyStr(c.ctx, root.value, "projection")};
    if (!JS_IsUndefined(projection.value)) {
        if (!object(projection.value) ||
            !keys(c.ctx, projection.value, {"pixelM", "paddingM", "hitEpsilonM", "normalEpsilonM"},
                  e))
            return fail(e, "invalid projection object");
        auto scalar = [&](const char *key, float &v, double lo, double hi) {
            double n = v;
            if (!number(c.ctx, projection.value, key, lo, hi, n, e))
                return false;
            v = float(n);
            return true;
        };
        if (!scalar("pixelM", result.pixel_m, .001, .006) ||
            !scalar("paddingM", result.padding_m, .001, .02) ||
            !scalar("hitEpsilonM", result.hit_epsilon_m, .000001, .00005) ||
            !scalar("normalEpsilonM", result.normal_epsilon_m, .000005, .0001))
            return false;
    }
    out = std::move(result);
    return true;
#endif
}
bool prepare_brick_bond_sources(const BrickBondDescriptor &d, uint64_t descriptor_hash,
                                const std::string &source_code, script_host::ScriptHost &host,
                                const script_host::SolidSourceEvaluationOptions &options,
                                PreparedBrickBond &out, std::string &error) {
#if !defined(MATTER_HAVE_SCRIPT_HOST)
    (void)d;
    (void)descriptor_hash;
    (void)source_code;
    (void)host;
    (void)options;
    (void)out;
    error = "brick bond source evaluation requires ScriptHost";
    return false;
#else
    PreparedBrickBond candidate;
    std::vector<uint64_t> identities{brick_bond_detail_version,
                                     castle_bake::brick_bond_recipe_digest(d.bond)};
    for (uint32_t seed = 0; seed < 8; ++seed) {
        const std::string params = host.merge_json_shallow(
            d.source_params_json, "{\"seed\":" + std::to_string(seed) + "}");
        script_host::BakeError evaluation;
        const auto evaluation_start = std::chrono::steady_clock::now();
        if (!host.evaluate_solid_source(source_code, params, candidate.sources[seed], evaluation,
                                        options)) {
            error = "source seed " + std::to_string(seed) + ": " + evaluation.message;
            return false;
        }
        candidate.source_evaluation_ms += std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - evaluation_start)
                                              .count();
        const auto &source = candidate.sources[seed];
        identities.push_back(source.resolved_hash);
        identities.push_back(source.recipe_digest);
        for (bool back : {false, true}) {
            const auto job = brick_bond_face_job(d, source.job(), source.resolved_hash, back);
            gpu_meshing::FaceLayout layout;
            gpu_meshing::Error validation;
            if (!gpu_meshing::validate_face_job(job, layout, validation)) {
                error = validation.message;
                return false;
            }
            identities.push_back(gpu_meshing::face_recipe_digest(job));
        }
    }
    candidate.cache_key =
        tileset::gtex_content_hash(castle_bake::brick_bond_recipe_digest(d.bond),
                                   tileset::gtex_script_identity_hash(descriptor_hash, identities));
    out = std::move(candidate);
    return true;
#endif
}
gpu_meshing::FaceJob brick_bond_face_job(const BrickBondDescriptor &d,
                                         const gpu_meshing::SolidJob &source, uint64_t identity,
                                         bool back) {
    gpu_meshing::FaceJob j;
    j.source = source;
    j.source_identity = identity;
    j.frame.origin_m = {0, d.bond.brick_height_m * .5f, 0};
    if (back) {
        j.frame.u = {-1, 0, 0};
        j.frame.n = {0, 0, -1};
    }
    j.u_min_m = -d.bond.brick_width_m * .5f - d.padding_m;
    j.u_max_m = -j.u_min_m;
    j.v_min_m = -d.bond.brick_height_m * .5f - d.padding_m;
    j.v_max_m = -j.v_min_m;
    j.height_max_m = d.bond.nominal_face_height_m + 2 * d.padding_m;
    j.height_min_m = -j.height_max_m;
    j.pixel_m = d.pixel_m;
    j.hit_epsilon_m = d.hit_epsilon_m;
    j.normal_epsilon_m = d.normal_epsilon_m;
    return j;
}
} // namespace detail_bake
