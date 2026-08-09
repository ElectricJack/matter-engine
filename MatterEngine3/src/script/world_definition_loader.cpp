#include "world_definition_loader.h"

#include "module_resolver.h"

extern "C" {
#include "quickjs.h"
#include "material_registry.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace matter {
namespace {

struct ModuleStore {
    std::map<std::string, std::string> sources;
};

std::string canonical_specifier(std::string specifier) {
    if (specifier.size() >= 3 &&
        specifier.compare(specifier.size() - 3, 3, ".js") == 0) {
        specifier.resize(specifier.size() - 3);
    }
    return specifier;
}

bool read_text_file(const std::string& path, std::string& contents) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream stream;
    stream << input.rdbuf();
    contents = stream.str();
    return input.good() || input.eof();
}

std::string join_path(const std::string& root, const std::string& leaf) {
    if (root.empty()) return {};
    const char last = root.back();
    return root + ((last == '/' || last == '\\') ? "" : "/") + leaf;
}

bool gather_modules(const std::string& world_source,
                    const WorldLoadDesc& desc,
                    ModuleStore& store,
                    std::string& message) {
    std::vector<std::string> work =
        module_resolver::parse_import_specifiers(world_source);
    std::set<std::string> visited;

    for (std::size_t index = 0; index < work.size(); ++index) {
        const std::string specifier = canonical_specifier(work[index]);
        if (!visited.insert(specifier).second) continue;

        constexpr const char* prefix = "shared-lib/";
        if (specifier.rfind(prefix, 0) != 0) {
            message = "specifier not under shared-lib/: " + specifier;
            return false;
        }
        const std::string name = specifier.substr(std::strlen(prefix));
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find("..") != std::string::npos) {
            message = "illegal shared-lib module name: " + name;
            return false;
        }

        std::string module_source;
        const std::string filename = name + ".js";
        const std::string project_path =
            join_path(desc.project_shared_lib_dir, filename);
        const std::string engine_path =
            join_path(desc.engine_shared_lib_dir, filename);
        if ((project_path.empty() || !read_text_file(project_path, module_source)) &&
            (engine_path.empty() || !read_text_file(engine_path, module_source))) {
            message = "module not found in project or engine shared-lib: " + specifier;
            return false;
        }

        store.sources.emplace(specifier, module_source);
        const std::vector<std::string> imports =
            module_resolver::parse_import_specifiers(module_source);
        work.insert(work.end(), imports.begin(), imports.end());
    }
    return true;
}

char* normalize_module(JSContext* context, const char*, const char* name, void*) {
    const std::string canonical = canonical_specifier(name ? name : "");
    char* result = static_cast<char*>(js_malloc(context, canonical.size() + 1));
    if (!result) return nullptr;
    std::memcpy(result, canonical.c_str(), canonical.size() + 1);
    return result;
}

JSModuleDef* load_module(JSContext* context, const char* name, void* opaque) {
    ModuleStore* store = static_cast<ModuleStore*>(opaque);
    if (!store) {
        JS_ThrowReferenceError(context, "module store is unavailable");
        return nullptr;
    }
    const auto found = store->sources.find(canonical_specifier(name ? name : ""));
    if (found == store->sources.end()) {
        JS_ThrowReferenceError(context, "module not in resolved shared-lib set: %s",
                               name ? name : "");
        return nullptr;
    }
    const std::string& source = found->second;
    JSValue compiled = JS_Eval(context, source.c_str(), source.size(), name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return nullptr;
    JSModuleDef* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
    JS_FreeValue(context, compiled);
    return module;
}

bool execute_jobs(JSRuntime* runtime, JSContext* context) {
    JSContext* job_context = context;
    for (;;) {
        const int result = JS_ExecutePendingJob(runtime, &job_context);
        if (result == 0) return true;
        if (result < 0) return false;
    }
}

JSContext* new_world_context(JSRuntime* runtime, bool modules) {
    JSContext* context = JS_NewContextRaw(runtime);
    if (!context) return nullptr;
    JS_AddIntrinsicBaseObjects(context);
    JS_AddIntrinsicEval(context);
    JS_AddIntrinsicRegExpCompiler(context);
    JS_AddIntrinsicRegExp(context);
    JS_AddIntrinsicJSON(context);
    JS_AddIntrinsicMapSet(context);
    JS_AddIntrinsicTypedArrays(context);
    JS_AddIntrinsicBigInt(context);
    if (modules) JS_AddIntrinsicPromise(context);
    return context;
}

std::string exception_message(JSContext* context) {
    JSValue exception = JS_GetException(context);
    JSValue stack = JS_GetPropertyStr(context, exception, "stack");
    const char* exception_text = JS_ToCString(context, exception);
    const char* stack_text = JS_IsString(stack) ? JS_ToCString(context, stack) : nullptr;
    std::string result = exception_text ? exception_text : "JavaScript exception";
    if (stack_text && result != stack_text) {
        result += '\n';
        result += stack_text;
    }
    if (stack_text) JS_FreeCString(context, stack_text);
    if (exception_text) JS_FreeCString(context, exception_text);
    JS_FreeValue(context, stack);
    JS_FreeValue(context, exception);
    return result;
}

bool fail(const WorldLoadDesc& desc,
          WorldLoadError& error,
          std::string property_path,
          std::string message) {
    error.message = std::move(message);
    error.source_location = desc.world_path;
    error.property_path = std::move(property_path);
    return false;
}

bool array_length(JSContext* context, JSValueConst value, std::uint32_t& length) {
    if (!JS_IsArray(value)) return false;
    JSValue length_value = JS_GetPropertyStr(context, value, "length");
    const int conversion = JS_ToUint32(context, &length, length_value);
    JS_FreeValue(context, length_value);
    return conversion == 0;
}

bool string_value(JSContext* context, JSValueConst value, std::string& output) {
    if (!JS_IsString(value)) return false;
    const char* text = JS_ToCString(context, value);
    if (!text) return false;
    output = text;
    JS_FreeCString(context, text);
    return true;
}

bool number_value(JSContext* context, JSValueConst value, float& output) {
    double number = 0.0;
    if (JS_ToFloat64(context, &number, value) < 0) return false;
    output = static_cast<float>(number);
    return true;
}

bool float3_value(JSContext* context, JSValueConst value, Float3& output) {
    std::uint32_t length = 0;
    if (!array_length(context, value, length) || length != 3) return false;
    float* coordinates[] = {&output.x, &output.y, &output.z};
    for (std::uint32_t index = 0; index < 3; ++index) {
        JSValue element = JS_GetPropertyUint32(context, value, index);
        const bool ok = number_value(context, element, *coordinates[index]);
        JS_FreeValue(context, element);
        if (!ok) return false;
    }
    return true;
}

bool float3_array_value(JSContext* context, JSValueConst value, float output[3]) {
    std::uint32_t length = 0;
    if (!array_length(context, value, length) || length != 3) return false;
    for (std::uint32_t index = 0; index < 3; ++index) {
        JSValue element = JS_GetPropertyUint32(context, value, index);
        const bool ok = number_value(context, element, output[index]);
        JS_FreeValue(context, element);
        if (!ok) return false;
    }
    return true;
}

bool canonical_json(JSContext* context,
                    JSValueConst canonicalizer,
                    JSValueConst value,
                    std::string& output) {
    JSValue argument = JS_DupValue(context, value);
    JSValue result = JS_Call(context, canonicalizer, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(context, argument);
    if (JS_IsException(result)) {
        JS_FreeValue(context, result);
        return false;
    }
    if (!JS_IsString(result)) {
        JS_FreeValue(context, result);
        return false;
    }
    const char* text = JS_ToCString(context, result);
    if (text) output = text;
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, result);
    return text != nullptr;
}

bool has_property(JSContext* context, JSValueConst object, const char* name) {
    JSAtom atom = JS_NewAtom(context, name);
    const int result = JS_HasProperty(context, object, atom);
    JS_FreeAtom(context, atom);
    return result == 1;
}

bool optional_number(JSContext* context,
                     JSValueConst object,
                     const char* name,
                     float& output) {
    JSValue value = JS_GetPropertyStr(context, object, name);
    const bool ok = JS_IsUndefined(value) || number_value(context, value, output);
    JS_FreeValue(context, value);
    return ok;
}

JSValue append_entity(JSContext* context,
                      JSValueConst,
                      int argument_count,
                      JSValueConst* arguments) {
    if (argument_count < 1) {
        return JS_ThrowTypeError(context, "entity(record) requires one record");
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue entities = JS_GetPropertyStr(context, global, "__matter_entities");
    JS_FreeValue(context, global);
    std::uint32_t count = 0;
    if (!array_length(context, entities, count)) {
        JS_FreeValue(context, entities);
        return JS_ThrowInternalError(context, "entity collection is unavailable");
    }
    const int result = JS_SetPropertyUint32(
        context, entities, count, JS_DupValue(context, arguments[0]));
    JS_FreeValue(context, entities);
    return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// defineMaterial(name, spec) — chart-VT spec Phase 3 / plan contract C3
// ---------------------------------------------------------------------------
// The binding runs during world-source evaluation, which happens before
// extract_roots(), so a root's params may reference a returned handle. After
// the roots are read the binding is replaced by a throwing stub (see
// install_define_material_epilogue) — a material defined from buildEntities()
// would be too late to schedule its detail bake, and silently doing nothing is
// the failure mode this diagnoses.
//
// The handle is the live registry index: defineMaterial calls
// MaterialRegistryDefineDynamic() immediately, so the value the script sees is
// the same id the renderer will index. That makes this loader the one place
// that mutates the registry's dynamic tail, and load_world_definition() resets
// that tail on entry so repeated loads are idempotent.

// Collector installed as the context opaque for the duration of a load. It
// carries everything a C-function binding needs to reach back into the
// in-progress WorldDefinition: the material list defineMaterial appends to,
// and the already-extracted `static props` specs getProp reads.
struct LoadCollector {
    std::vector<WorldMaterial>* materials = nullptr;
    const std::vector<WorldPropSpec>* props = nullptr;
};

std::uint32_t fnv1a32(const std::string& text) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

bool spec_number(JSContext* context, JSValueConst spec, const char* key,
                 float& output, bool& present) {
    JSValue value = JS_GetPropertyStr(context, spec, key);
    present = !JS_IsUndefined(value);
    const bool ok = !present || number_value(context, value, output);
    JS_FreeValue(context, value);
    return ok;
}

bool spec_int(JSContext* context, JSValueConst spec, const char* key,
              int& output, bool& present) {
    float number = 0.0f;
    if (!spec_number(context, spec, key, number, present)) return false;
    if (present) {
        if (!std::isfinite(number)) return false;
        output = static_cast<int>(number);
    }
    return true;
}

bool spec_float3(JSContext* context, JSValueConst spec, const char* key,
                 float output[3], bool& present) {
    JSValue value = JS_GetPropertyStr(context, spec, key);
    present = !JS_IsUndefined(value);
    const bool ok = !present || float3_array_value(context, value, output);
    JS_FreeValue(context, value);
    return ok;
}

// Optional boolean that folds into MaterialDef::surfaceFlags.
bool spec_flag(JSContext* context, JSValueConst spec, const char* key,
               std::uint32_t bit, std::uint32_t& flags) {
    JSValue value = JS_GetPropertyStr(context, spec, key);
    const bool present = !JS_IsUndefined(value);
    if (present && JS_ToBool(context, value) != 0) flags |= bit;
    JS_FreeValue(context, value);
    return true;
}

const char* const kMaterialSpecKeys[] = {
    "albedo", "roughness", "metallic", "emission", "translucency", "ior",
    "flatShading", "mergeGroup", "meshingAlgorithm", "opacity", "transmission",
    "emissionColor", "absorptionColor", "absorptionDistance", "thickness",
    "subsurface", "scatteringColor", "scatteringDistance", "anisotropy",
    "clearcoat", "clearcoatRoughness", "specularStrength", "specularTint",
    "alphaCutoff", "shadowOpacity",
    "thinWalled", "doubleSided", "alphaTested", "volumeBoundary",
    "detail", "detailDensity",
};

// A typo in a spec key would otherwise shade with a silently-defaulted value,
// which is exactly the class of bug this authoring surface exists to remove.
// `keys` is the allow-list; the caller names it so the material spec and the
// `static props` spec share one strictness rule without sharing a vocabulary.
bool reject_unknown_keys(JSContext* context, JSValueConst spec,
                         const char* const* keys, std::size_t key_count,
                         std::string& unknown) {
    JSPropertyEnum* properties = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &count, spec,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
        return false;
    bool ok = true;
    for (std::uint32_t index = 0; index < count && ok; ++index) {
        const char* text = JS_AtomToCString(context, properties[index].atom);
        if (!text) { ok = false; break; }
        bool known = false;
        for (std::size_t key = 0; key < key_count; ++key)
            if (std::strcmp(keys[key], text) == 0) { known = true; break; }
        if (!known) { unknown = text; ok = false; }
        JS_FreeCString(context, text);
    }
    JS_FreePropertyEnum(context, properties, count);
    return ok;
}

bool reject_unknown_spec_keys(JSContext* context, JSValueConst spec,
                              std::string& unknown) {
    return reject_unknown_keys(context, spec, kMaterialSpecKeys,
                               sizeof(kMaterialSpecKeys) / sizeof(kMaterialSpecKeys[0]),
                               unknown);
}

JSValue define_material(JSContext* context,
                        JSValueConst,
                        int argument_count,
                        JSValueConst* arguments) {
    LoadCollector* collector =
        static_cast<LoadCollector*>(JS_GetContextOpaque(context));
    if (!collector || !collector->materials)
        return JS_ThrowInternalError(context, "material collector unavailable");

    std::string name;
    if (argument_count < 1 || !string_value(context, arguments[0], name) ||
        name.empty()) {
        return JS_ThrowTypeError(
            context, "defineMaterial(name, spec): name must be a non-empty string");
    }
    if (name.size() + 1 > static_cast<std::size_t>(MATERIAL_NAME_MAX)) {
        return JS_ThrowTypeError(context,
                                 "defineMaterial: name '%s' exceeds %d characters",
                                 name.c_str(), MATERIAL_NAME_MAX - 1);
    }

    const bool has_spec = argument_count >= 2 && !JS_IsUndefined(arguments[1]) &&
                          !JS_IsNull(arguments[1]);
    if (has_spec && !JS_IsObject(arguments[1])) {
        return JS_ThrowTypeError(
            context, "defineMaterial('%s'): spec must be an object", name.c_str());
    }
    JSValue spec = has_spec ? JS_DupValue(context, arguments[1])
                            : JS_NewObject(context);

    std::string unknown_key;
    if (!reject_unknown_spec_keys(context, spec, unknown_key)) {
        JS_FreeValue(context, spec);
        if (!unknown_key.empty())
            return JS_ThrowTypeError(context,
                                     "defineMaterial('%s'): unknown spec field '%s'",
                                     name.c_str(), unknown_key.c_str());
        return JS_EXCEPTION;
    }

    MaterialDef def{};
    MaterialRegistryDefaultDynamicDef(&def);
    // Distinct dynamic materials must not share a merge group with each other
    // or with a builtin (0..25), or the SDF mesher would blend them. Derived
    // from the name so the group is stable across loads and independent of
    // declaration order.
    def.mergeGroup = 1000 + static_cast<int>(fnv1a32(name) % 1000000u);

    bool present = false;
    bool ok = spec_float3(context, spec, "albedo", def.albedo, present) &&
              spec_number(context, spec, "roughness", def.roughness, present) &&
              spec_number(context, spec, "metallic", def.metallic, present) &&
              spec_number(context, spec, "emission", def.emission, present) &&
              spec_number(context, spec, "translucency", def.translucency, present) &&
              spec_number(context, spec, "ior", def.ior, present) &&
              spec_number(context, spec, "opacity", def.opacity, present) &&
              spec_number(context, spec, "transmission", def.transmission, present) &&
              spec_float3(context, spec, "emissionColor", def.emissionColor, present) &&
              spec_float3(context, spec, "absorptionColor", def.absorptionColor, present) &&
              spec_number(context, spec, "absorptionDistance", def.absorptionDistance, present) &&
              spec_number(context, spec, "thickness", def.thickness, present) &&
              spec_number(context, spec, "subsurface", def.subsurface, present) &&
              spec_float3(context, spec, "scatteringColor", def.scatteringColor, present) &&
              spec_number(context, spec, "scatteringDistance", def.scatteringDistance, present) &&
              spec_number(context, spec, "anisotropy", def.anisotropy, present) &&
              spec_number(context, spec, "clearcoat", def.clearcoat, present) &&
              spec_number(context, spec, "clearcoatRoughness", def.clearcoatRoughness, present) &&
              spec_number(context, spec, "specularStrength", def.specularStrength, present) &&
              spec_float3(context, spec, "specularTint", def.specularTint, present) &&
              spec_number(context, spec, "alphaCutoff", def.alphaCutoff, present) &&
              spec_number(context, spec, "shadowOpacity", def.shadowOpacity, present) &&
              spec_int(context, spec, "flatShading", def.flatShading, present) &&
              spec_int(context, spec, "mergeGroup", def.mergeGroup, present) &&
              spec_int(context, spec, "meshingAlgorithm", def.meshingAlgorithm, present);
    if (!ok) {
        JS_FreeValue(context, spec);
        return JS_ThrowTypeError(
            context,
            "defineMaterial('%s'): numeric fields must be finite numbers and "
            "color fields must contain 3 numbers",
            name.c_str());
    }

    std::uint32_t flags = def.surfaceFlags;
    spec_flag(context, spec, "thinWalled", MATERIAL_THIN_WALLED, flags);
    spec_flag(context, spec, "doubleSided", MATERIAL_DOUBLE_SIDED, flags);
    spec_flag(context, spec, "alphaTested", MATERIAL_ALPHA_TESTED, flags);
    spec_flag(context, spec, "volumeBoundary", MATERIAL_VOLUME_BOUNDARY, flags);
    def.surfaceFlags = flags;

    WorldMaterial record;
    record.name = name;

    JSValue detail = JS_GetPropertyStr(context, spec, "detail");
    if (!JS_IsUndefined(detail) &&
        (!string_value(context, detail, record.detail_module) ||
         record.detail_module.empty())) {
        JS_FreeValue(context, detail);
        JS_FreeValue(context, spec);
        return JS_ThrowTypeError(
            context, "defineMaterial('%s'): detail must be a Tileset module name",
            name.c_str());
    }
    JS_FreeValue(context, detail);

    float density = 0.0f;
    bool density_present = false;
    if (!spec_number(context, spec, "detailDensity", density, density_present) ||
        (density_present && (!std::isfinite(density) || density <= 0.0f))) {
        JS_FreeValue(context, spec);
        return JS_ThrowTypeError(
            context, "defineMaterial('%s'): detailDensity must be a positive number",
            name.c_str());
    }
    if (density_present) record.detail_density = static_cast<int>(density);
    if (density_present && record.detail_module.empty()) {
        JS_FreeValue(context, spec);
        return JS_ThrowTypeError(
            context,
            "defineMaterial('%s'): detailDensity requires a detail tileset",
            name.c_str());
    }
    JS_FreeValue(context, spec);

    const int handle = MaterialRegistryDefineDynamic(&def, name.c_str());
    if (handle == MATERIAL_DEFINE_ERR_CONFLICT) {
        return JS_ThrowTypeError(
            context,
            "defineMaterial('%s'): already defined with a different spec; a "
            "repeated definition must be identical",
            name.c_str());
    }
    if (handle == MATERIAL_DEFINE_ERR_FULL) {
        return JS_ThrowRangeError(
            context,
            "defineMaterial('%s'): material registry is full (%d entries max)",
            name.c_str(), MATERIAL_MAX_TOTAL);
    }
    if (handle < 0) {
        return JS_ThrowTypeError(context, "defineMaterial('%s'): rejected",
                                 name.c_str());
    }

    record.index = handle;
    // A repeated identical definition returns the same handle; keep exactly one
    // record per material so the provider does not schedule the bake twice.
    for (WorldMaterial& existing : *collector->materials) {
        if (existing.index != handle) continue;
        if (existing.detail_module != record.detail_module ||
            existing.detail_density != record.detail_density) {
            return JS_ThrowTypeError(
                context,
                "defineMaterial('%s'): repeated definition changes the detail "
                "tileset",
                name.c_str());
        }
        return JS_NewInt32(context, handle);
    }
    collector->materials->push_back(std::move(record));
    return JS_NewInt32(context, handle);
}

JSValue define_material_too_late(JSContext* context,
                                 JSValueConst,
                                 int,
                                 JSValueConst*) {
    return JS_ThrowTypeError(
        context,
        "defineMaterial must be called while the world module evaluates "
        "(module scope or a class static), before World.roots is read — a "
        "material declared later cannot schedule its detail-tileset bake");
}

bool extract_settings_object(JSContext* context,
                             JSValueConst object,
                             WorldSettings& settings) {
    return optional_number(context, object, "sectorSize", settings.sector_size) &&
           optional_number(context, object, "yMin", settings.y_min) &&
           optional_number(context, object, "yMax", settings.y_max);
}

bool extract_roots(JSContext* context,
                   JSValueConst world_class,
                   JSValueConst canonicalizer,
                   const WorldLoadDesc& desc,
                   WorldDefinition& definition,
                   WorldLoadError& error) {
    JSValue roots = JS_GetPropertyStr(context, world_class, "roots");
    if (JS_IsUndefined(roots)) {
        JS_FreeValue(context, roots);
        return true;
    }
    std::uint32_t count = 0;
    if (!array_length(context, roots, count)) {
        JS_FreeValue(context, roots);
        return fail(desc, error, "roots", "World.roots must be an array");
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::string path = "roots[" + std::to_string(index) + "]";
        JSValue entry = JS_GetPropertyUint32(context, roots, index);
        if (!JS_IsObject(entry)) {
            JS_FreeValue(context, entry);
            JS_FreeValue(context, roots);
            return fail(desc, error, path, "world root must be an object");
        }
        WorldRoot root;
        JSValue module = JS_GetPropertyStr(context, entry, "module");
        if (!string_value(context, module, root.module)) {
            JS_FreeValue(context, module);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, roots);
            return fail(desc, error, path + ".module",
                        "world root module must be a string");
        }
        JS_FreeValue(context, module);

        const bool params_present = has_property(context, entry, "params");
        JSValue params = JS_GetPropertyStr(context, entry, "params");
        if (params_present &&
            !canonical_json(context, canonicalizer, params, root.params_json)) {
            JS_FreeValue(context, params);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, roots);
            return fail(desc, error, path + ".params",
                        "world root params must be JSON serializable");
        }
        JS_FreeValue(context, params);

        JSValue transform = JS_GetPropertyStr(context, entry, "transform");
        if (!JS_IsUndefined(transform)) {
            std::uint32_t length = 0;
            if (!array_length(context, transform, length) || length != 16) {
                JS_FreeValue(context, transform);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".transform",
                            "world root transform must contain 16 numbers");
            }
            for (std::uint32_t element_index = 0; element_index < 16; ++element_index) {
                JSValue element = JS_GetPropertyUint32(context, transform, element_index);
                const bool ok = number_value(context, element, root.transform.m[element_index]);
                JS_FreeValue(context, element);
                if (!ok) {
                    JS_FreeValue(context, transform);
                    JS_FreeValue(context, entry);
                    JS_FreeValue(context, roots);
                    return fail(desc, error,
                                path + ".transform[" +
                                    std::to_string(element_index) + "]",
                                "world root transform value must be numeric");
                }
            }
        }
        JS_FreeValue(context, transform);

        JSValue expand = JS_GetPropertyStr(context, entry, "expand");
        if (!JS_IsUndefined(expand)) root.expand = JS_ToBool(context, expand) != 0;
        JS_FreeValue(context, expand);
        JSValue tileset = JS_GetPropertyStr(context, entry, "tileset");
        if (!JS_IsUndefined(tileset)) root.tileset = JS_ToBool(context, tileset) != 0;
        JS_FreeValue(context, tileset);

        definition.roots.push_back(std::move(root));
        JS_FreeValue(context, entry);
    }
    JS_FreeValue(context, roots);
    return true;
}

bool extract_lights(JSContext* context,
                    JSValueConst world_class,
                    const WorldLoadDesc& desc,
                    WorldDefinition& definition,
                    WorldLoadError& error) {
    JSValue lights = JS_GetPropertyStr(context, world_class, "lights");
    if (JS_IsUndefined(lights)) {
        JS_FreeValue(context, lights);
        return true;
    }

    std::uint32_t count = 0;
    if (array_length(context, lights, count)) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::string path = "lights[" + std::to_string(index) + "]";
            JSValue entry = JS_GetPropertyUint32(context, lights, index);
            WorldLight light;
            JSValue position = JS_GetPropertyStr(context, entry, "position");
            if (!float3_value(context, position, light.position)) {
                JS_FreeValue(context, position);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, lights);
                return fail(desc, error, path + ".position",
                            "light position must contain 3 numbers");
            }
            JS_FreeValue(context, position);
            JSValue color = JS_GetPropertyStr(context, entry, "color");
            if (!JS_IsUndefined(color) && !float3_value(context, color, light.color)) {
                JS_FreeValue(context, color);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, lights);
                return fail(desc, error, path + ".color",
                            "light color must contain 3 numbers");
            }
            JS_FreeValue(context, color);
            if (!optional_number(context, entry, "intensity", light.intensity) ||
                !optional_number(context, entry, "range", light.range)) {
                JS_FreeValue(context, entry);
                JS_FreeValue(context, lights);
                return fail(desc, error, path,
                            "light intensity and range must be numeric");
            }
            definition.lights.push_back(light);
            JS_FreeValue(context, entry);
        }
        JS_FreeValue(context, lights);
        return true;
    }

    // Compatibility with the approved World-as-JS sun/sky object. These map
    // directly onto established renderer settings without duplicating its type.
    if (!JS_IsObject(lights)) {
        JS_FreeValue(context, lights);
        return fail(desc, error, "lights", "World.lights must be an array or object");
    }
    JSValue sun = JS_GetPropertyStr(context, lights, "sun");
    if (!JS_IsUndefined(sun)) {
        // Two spellings for the same thing. `dir` is the original and stays
        // exactly as it was — a Float3 pointing FROM the sun TOWARD the scene.
        // `azimuth`/`elevation` are the human ones (matter/sun_angles.h owns
        // the convention and the conversion); when either is present it WINS,
        // because a script that says elevation: 15 means it.
        //
        // Both remain optional now. They were both mandatory only because
        // float3_value rejects undefined, which made `sun: { azimuth: 15 }`
        // impossible to write; every world that used to parse still parses.
        const bool has_dir = has_property(context, sun, "dir");
        const bool has_azimuth = has_property(context, sun, "azimuth");
        const bool has_elevation = has_property(context, sun, "elevation");
        bool ok = true;
        if (has_dir) {
            JSValue direction = JS_GetPropertyStr(context, sun, "dir");
            ok = float3_value(context, direction, definition.settings.sun_direction);
            JS_FreeValue(context, direction);
        }
        if (ok && has_property(context, sun, "color")) {
            JSValue color = JS_GetPropertyStr(context, sun, "color");
            ok = float3_value(context, color, definition.settings.sun_color);
            JS_FreeValue(context, color);
        }
        if (ok && (has_azimuth || has_elevation)) {
            // Seed from whatever direction is in force (authored `dir` above,
            // or the compiled default) so a script may set just one angle.
            float azimuth = 0.0f, elevation = 0.0f;
            sun_angles_from_direction(definition.settings.sun_direction,
                                      azimuth, elevation);
            ok = optional_number(context, sun, "azimuth", azimuth) &&
                 optional_number(context, sun, "elevation", elevation);
            // JS_ToFloat64 SUCCEEDS on a string and hands back NaN, so
            // optional_number alone would let `azimuth: 'south'` through and
            // put a NaN light vector in the manifest, where nothing downstream
            // recovers from it. Check finiteness here rather than in
            // number_value: every other optional_number caller has lived with
            // that behaviour for a long time and this is not the change to
            // alter it under them.
            ok = ok && std::isfinite(azimuth) && std::isfinite(elevation);
            if (ok)
                definition.settings.sun_direction =
                    sun_direction_from_angles(azimuth, elevation);
        }
        if (ok) {
            // Angular diameter in degrees. `size` is the short spelling;
            // `angularDiameter` matches the struct field for scripts that
            // prefer being explicit.
            ok = optional_number(context, sun, "size",
                                 definition.settings.sun_angular_diameter_deg) &&
                 optional_number(context, sun, "angularDiameter",
                                 definition.settings.sun_angular_diameter_deg) &&
                 std::isfinite(definition.settings.sun_angular_diameter_deg);
        }
        if (!ok) {
            JS_FreeValue(context, sun);
            JS_FreeValue(context, lights);
            return fail(desc, error, "lights.sun",
                        "sun.dir and sun.color must contain 3 numbers; "
                        "sun.azimuth, sun.elevation and sun.size must be "
                        "numbers");
        }
    }
    JS_FreeValue(context, sun);
    JSValue sky = JS_GetPropertyStr(context, lights, "sky");
    if (!JS_IsUndefined(sky)) {
        JSValue color = JS_GetPropertyStr(context, sky, "color");
        const bool ok = float3_value(context, color, definition.settings.sky_color);
        JS_FreeValue(context, color);
        if (!ok) {
            JS_FreeValue(context, sky);
            JS_FreeValue(context, lights);
            return fail(desc, error, "lights.sky.color",
                        "sky.color must contain 3 numbers");
        }
    }
    JS_FreeValue(context, sky);

    JSValue spots = JS_GetPropertyStr(context, lights, "spots");
    if (!JS_IsUndefined(spots)) {
        std::uint32_t spot_count = 0;
        if (!array_length(context, spots, spot_count)) {
            JS_FreeValue(context, spots);
            JS_FreeValue(context, lights);
            return fail(desc, error, "lights.spots", "lights.spots must be an array");
        }
        for (std::uint32_t index = 0; index < spot_count; ++index) {
            const std::string path = "lights.spots[" + std::to_string(index) + "]";
            JSValue entry = JS_GetPropertyUint32(context, spots, index);
            WorldLight light;
            JSValue position = JS_GetPropertyStr(context, entry, "position");
            if (JS_IsUndefined(position)) {
                JS_FreeValue(context, position);
                position = JS_GetPropertyStr(context, entry, "pos");
            }
            JSValue direction = JS_GetPropertyStr(context, entry, "direction");
            if (JS_IsUndefined(direction)) {
                JS_FreeValue(context, direction);
                direction = JS_GetPropertyStr(context, entry, "dir");
            }
            JSValue color = JS_GetPropertyStr(context, entry, "color");
            const bool vectors_ok = float3_value(context, position, light.position) &&
                                    float3_value(context, direction, light.direction) &&
                                    float3_value(context, color, light.color);
            JS_FreeValue(context, position);
            JS_FreeValue(context, direction);
            JS_FreeValue(context, color);
            const bool numbers_ok =
                optional_number(context, entry, "intensity", light.intensity) &&
                optional_number(context, entry, "range", light.range) &&
                optional_number(context, entry, "inner", light.inner_cone_degrees) &&
                optional_number(context, entry, "outer", light.outer_cone_degrees);
            JS_FreeValue(context, entry);
            if (!vectors_ok || !numbers_ok) {
                JS_FreeValue(context, spots);
                JS_FreeValue(context, lights);
                return fail(desc, error, path,
                            "spot position/direction/color and numeric range/cones are required");
            }
            definition.lights.push_back(light);
        }
    }
    JS_FreeValue(context, spots);
    JS_FreeValue(context, lights);
    return true;
}

// `fog.clouds`: an array of up to kMaxCloudLayers bounded decks.
//
//   static fog = {
//     density: 0.004, floor: 0, falloff: 90,
//     clouds: [
//       { minHeight: 140, maxHeight: 210, maxDensity: 0.03,
//         falloffMin: 12, falloffMax: 40, noiseScale: 0.0016,
//         octaves: 2, lacunarity: 2.03, gain: 0.5, coverage: 0.55,
//         wind: [1.2, 0, 0.3] },
//       ...
//     ],
//   };
//
// Every key is optional except the two heights, which have no sane default —
// a deck has to be told where it is. Ranges are clamped by
// sanitize_cloud_layer rather than rejected, EXCEPT max <= min, which is
// rejected: that is a typo, not a taste, and silently disabling the layer
// would leave the author staring at a clear sky wondering why.
bool extract_cloud_layers(JSContext* context, JSValueConst fog_val,
                          const WorldLoadDesc& desc, FogSettings& fog,
                          WorldLoadError& error) {
    JSValue clouds = JS_GetPropertyStr(context, fog_val, "clouds");
    if (JS_IsUndefined(clouds)) {
        JS_FreeValue(context, clouds);
        return true;  // legacy alias handled later — see apply_legacy_height_layer
    }
    if (!JS_IsArray(clouds)) {
        JS_FreeValue(context, clouds);
        return fail(desc, error, "fog.clouds", "fog.clouds must be an array");
    }

    std::uint32_t count = 0;
    if (!array_length(context, clouds, count)) {
        JS_FreeValue(context, clouds);
        return fail(desc, error, "fog.clouds", "fog.clouds must be an array");
    }
    if (count > static_cast<std::uint32_t>(kMaxCloudLayers)) {
        JS_FreeValue(context, clouds);
        return fail(desc, error, "fog.clouds",
                    "at most " + std::to_string(kMaxCloudLayers) +
                        " cloud layers are supported");
    }

    int32_t written = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        JSValue entry = JS_GetPropertyUint32(context, clouds, i);
        if (!JS_IsObject(entry)) {
            JS_FreeValue(context, entry);
            JS_FreeValue(context, clouds);
            return fail(desc, error,
                        "fog.clouds[" + std::to_string(i) + "]",
                        "each cloud layer must be an object");
        }
        CloudLayer layer{};
        layer.enabled = true;
        const struct { const char* name; float* target; } numbers[] = {
            {"minHeight", &layer.min_height},
            {"maxHeight", &layer.max_height},
            {"maxDensity", &layer.max_density},
            {"falloffMin", &layer.falloff_min},
            {"falloffMax", &layer.falloff_max},
            {"noiseScale", &layer.noise_scale},
            {"lacunarity", &layer.lacunarity},
            {"gain", &layer.gain},
            {"coverage", &layer.coverage},
        };
        bool ok = true;
        std::string bad_key;
        for (const auto& n : numbers) {
            if (!optional_number(context, entry, n.name, *n.target) ||
                !std::isfinite(*n.target)) {
                ok = false;
                bad_key = n.name;
                break;
            }
        }
        float octaves = static_cast<float>(layer.octaves);
        if (ok && (!optional_number(context, entry, "octaves", octaves) ||
                   !std::isfinite(octaves))) {
            ok = false;
            bad_key = "octaves";
        }
        layer.octaves = static_cast<int32_t>(octaves);
        JSValue wind = JS_GetPropertyStr(context, entry, "wind");
        if (ok && !JS_IsUndefined(wind) &&
            !float3_array_value(context, wind, layer.wind)) {
            ok = false;
            bad_key = "wind";
        }
        JS_FreeValue(context, wind);
        JS_FreeValue(context, entry);
        if (!ok) {
            JS_FreeValue(context, clouds);
            return fail(desc, error,
                        "fog.clouds[" + std::to_string(i) + "]." + bad_key,
                        "must be a finite number (wind: three of them)");
        }
        if (!(layer.max_height > layer.min_height)) {
            JS_FreeValue(context, clouds);
            return fail(desc, error,
                        "fog.clouds[" + std::to_string(i) + "]",
                        "maxHeight must be greater than minHeight");
        }
        sanitize_cloud_layer(layer);
        fog.clouds[written++] = layer;
    }
    JS_FreeValue(context, clouds);
    fog.cloud_count = written;
    // An explicit array supersedes the legacy alias entirely, including its
    // "density is the cloud's" convention — a world spelling both gets the
    // ground fog its `density` now unambiguously means.
    fog.height_layer = false;
    compact_clouds(fog);
    return true;
}

bool extract_fog(JSContext* context,
                 JSValueConst world_class,
                 const WorldLoadDesc& desc,
                 WorldDefinition& definition,
                 WorldLoadError& error) {
    JSValue fog_val = JS_GetPropertyStr(context, world_class, "fog");
    if (JS_IsUndefined(fog_val)) {
        JS_FreeValue(context, fog_val);
        return true;
    }
    if (!JS_IsObject(fog_val)) {
        JS_FreeValue(context, fog_val);
        return fail(desc, error, "fog", "World.fog must be an object");
    }

    FogSettings& fog = definition.settings.fog;

    if (!optional_number(context, fog_val, "density", fog.density) ||
        !optional_number(context, fog_val, "floor", fog.floor) ||
        !optional_number(context, fog_val, "falloff", fog.falloff)) {
        JS_FreeValue(context, fog_val);
        return fail(desc, error, "fog",
                    "fog density, floor, and falloff must be numeric");
    }

    const bool has_min_height = has_property(context, fog_val, "minHeight");
    const bool has_max_height = has_property(context, fog_val, "maxHeight");
    if (has_min_height != has_max_height) {
        JS_FreeValue(context, fog_val);
        return fail(desc, error, "fog",
                    "fog minHeight and maxHeight must be authored together");
    }
    if (has_min_height) {
        if (!optional_number(context, fog_val, "minHeight", fog.min_height) ||
            !optional_number(context, fog_val, "maxHeight", fog.max_height) ||
            !optional_number(context, fog_val, "noiseScale", fog.noise_scale) ||
            !std::isfinite(fog.min_height) ||
            !std::isfinite(fog.max_height) ||
            !std::isfinite(fog.noise_scale) ||
            fog.max_height <= fog.min_height ||
            fog.noise_scale <= 0.0f) {
            JS_FreeValue(context, fog_val);
            return fail(
                desc, error, "fog",
                "fog height layer requires maxHeight > minHeight and a "
                "positive noiseScale");
        }
        fog.height_layer = true;
    }

    // `fog.clouds` — the current spelling, an array of bounded decks. Parsed
    // AFTER the legacy keys so a world carrying both wins with the explicit
    // form rather than the alias.
    if (!extract_cloud_layers(context, fog_val, desc, fog, error)) {
        JS_FreeValue(context, fog_val);
        return false;
    }

    JSValue color = JS_GetPropertyStr(context, fog_val, "color");
    if (!JS_IsUndefined(color) && !float3_array_value(context, color, fog.color)) {
        JS_FreeValue(context, color);
        JS_FreeValue(context, fog_val);
        return fail(desc, error, "fog.color",
                    "fog.color must contain 3 numbers");
    }
    JS_FreeValue(context, color);

    JSValue wind = JS_GetPropertyStr(context, fog_val, "wind");
    if (!JS_IsUndefined(wind) && !float3_array_value(context, wind, fog.wind)) {
        JS_FreeValue(context, wind);
        JS_FreeValue(context, fog_val);
        return fail(desc, error, "fog.wind",
                    "fog.wind must contain 3 numbers");
    }
    JS_FreeValue(context, wind);

    JS_FreeValue(context, fog_val);
    return true;
}

bool extract_camera(JSContext* context,
                    JSValueConst world_class,
                    const WorldLoadDesc& desc,
                    WorldDefinition& definition,
                    WorldLoadError& error) {
    JSValue camera = JS_GetPropertyStr(context, world_class, "camera");
    if (JS_IsUndefined(camera)) {
        JS_FreeValue(context, camera);
        return true;
    }
    if (!JS_IsObject(camera)) {
        JS_FreeValue(context, camera);
        return fail(desc, error, "camera", "World.camera must be an object");
    }

    JSValue position = JS_GetPropertyStr(context, camera, "position");
    JSValue target = JS_GetPropertyStr(context, camera, "target");
    WorldCameraSettings authored;
    const bool vectors_ok =
        float3_value(context, position, authored.position) &&
        float3_value(context, target, authored.target);
    JS_FreeValue(context, position);
    JS_FreeValue(context, target);
    JS_FreeValue(context, camera);

    const Float3 delta{
        authored.target.x - authored.position.x,
        authored.target.y - authored.position.y,
        authored.target.z - authored.position.z};
    const bool finite =
        std::isfinite(authored.position.x) &&
        std::isfinite(authored.position.y) &&
        std::isfinite(authored.position.z) &&
        std::isfinite(authored.target.x) &&
        std::isfinite(authored.target.y) &&
        std::isfinite(authored.target.z);
    const float distance_squared =
        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (!vectors_ok || !finite || distance_squared <= 1e-6f) {
        return fail(
            desc, error, "camera",
            "camera position and target must be distinct finite float3 arrays");
    }

    authored.authored = true;
    definition.settings.camera = authored;
    return true;
}

bool extract_streaming(JSContext* context,
                       JSValueConst world_class,
                       const WorldLoadDesc& desc,
                       WorldDefinition& definition,
                       WorldLoadError& error) {
    JSValue streaming = JS_GetPropertyStr(context, world_class, "streaming");
    if (JS_IsUndefined(streaming)) {
        JS_FreeValue(context, streaming);
        return true;
    }
    if (!JS_IsObject(streaming)) {
        JS_FreeValue(context, streaming);
        return fail(desc, error, "streaming",
                    "World.streaming must be an object");
    }

    // Nested sector LOD (docs/terrain-nested-sector-lod-2026-08-08.md). Read
    // BEFORE the rings early-out below: nesting reinterprets `terrainBands` as
    // per-level annuli and makes the outermost BAND (not the outermost ring)
    // bound residency, so a world can legitimately declare nesting and bands
    // without declaring rings at all, and parsing this after the early return
    // would silently drop the flag for exactly those worlds.
    {
        JSValue nested =
            JS_GetPropertyStr(context, streaming, "nestedSectors");
        if (!JS_IsUndefined(nested))
            definition.settings.nested_sectors =
                JS_ToBool(context, nested) != 0;
        JS_FreeValue(context, nested);
    }

    // Rings are OPTIONAL, and an absent `rings` used to return from this whole
    // function -- taking `terrainBands` with it. A world that authored bands
    // and no rings therefore lost its band table silently. That was survivable
    // while the outermost ring bounded residency (no rings, nothing streamed
    // anyway); under nested sector LOD the outermost BAND bounds residency, so
    // such a world is meaningful and must keep its bands.
    JSValue rings = JS_GetPropertyStr(context, streaming, "rings");
    if (JS_IsUndefined(rings)) {
        JS_FreeValue(context, rings);
        rings = JS_UNDEFINED;
    } else {
    std::uint32_t count = 0;
    if (!array_length(context, rings, count) || count == 0) {
        JS_FreeValue(context, rings);
        JS_FreeValue(context, streaming);
        return fail(desc, error, "streaming.rings",
                    "streaming.rings must be a non-empty array");
    }

    float previous_radius = 0.0f;
    int previous_rung = -1;
    std::vector<WorldStreamingRing> parsed;
    parsed.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        JSValue entry = JS_GetPropertyUint32(context, rings, index);
        if (!JS_IsObject(entry)) {
            JS_FreeValue(context, entry);
            JS_FreeValue(context, rings);
            JS_FreeValue(context, streaming);
            return fail(
                desc, error,
                "streaming.rings[" + std::to_string(index) + "]",
                "each streaming ring must be an object");
        }
        JSValue radius_value = JS_GetPropertyStr(context, entry, "radius");
        JSValue rung_value = JS_GetPropertyStr(context, entry, "rung");
        float radius = 0.0f;
        float rung_number = -1.0f;
        const bool valid_values =
            number_value(context, radius_value, radius) &&
            number_value(context, rung_value, rung_number);
        JS_FreeValue(context, radius_value);
        JS_FreeValue(context, rung_value);
        JS_FreeValue(context, entry);

        const int rung = int(rung_number);
        const bool ordered =
            std::isfinite(radius) && radius > previous_radius && rung >= 0 &&
            std::isfinite(rung_number) && rung_number == float(rung) &&
            (index == 0 || rung == previous_rung - 1);
        if (!valid_values || !ordered) {
            JS_FreeValue(context, rings);
            JS_FreeValue(context, streaming);
            return fail(
                desc, error,
                "streaming.rings[" + std::to_string(index) + "]",
                "rings require increasing positive radii and consecutive "
                "descending non-negative rungs");
        }
        parsed.push_back({radius, int(rung)});
        previous_radius = radius;
        previous_rung = int(rung);
    }

    definition.settings.streaming_rings = std::move(parsed);
    JS_FreeValue(context, rings);
    }

    // Optional heightfield terrain LOD bands: same shape/ordering contract
    // as rings ({radius, lod}, increasing radii, consecutive descending
    // LODs), innermost band = finest level.
    JSValue bands = JS_GetPropertyStr(context, streaming, "terrainBands");
    if (!JS_IsUndefined(bands)) {
        std::uint32_t band_count = 0;
        if (!array_length(context, bands, band_count) || band_count == 0) {
            JS_FreeValue(context, bands);
            JS_FreeValue(context, streaming);
            return fail(desc, error, "streaming.terrainBands",
                        "streaming.terrainBands must be a non-empty array");
        }
        float prev_radius = 0.0f;
        int prev_lod = -1;
        std::vector<WorldStreamingRing> parsed_bands;
        parsed_bands.reserve(band_count);
        for (std::uint32_t index = 0; index < band_count; ++index) {
            JSValue entry = JS_GetPropertyUint32(context, bands, index);
            JSValue radius_value =
                JS_IsObject(entry) ? JS_GetPropertyStr(context, entry, "radius")
                                   : JS_UNDEFINED;
            JSValue lod_value =
                JS_IsObject(entry) ? JS_GetPropertyStr(context, entry, "lod")
                                   : JS_UNDEFINED;
            float radius = 0.0f;
            float lod_number = -1.0f;
            const bool valid_values =
                JS_IsObject(entry) &&
                number_value(context, radius_value, radius) &&
                number_value(context, lod_value, lod_number);
            JS_FreeValue(context, radius_value);
            JS_FreeValue(context, lod_value);
            JS_FreeValue(context, entry);

            const int lod = int(lod_number);
            const bool ordered =
                std::isfinite(radius) && radius > prev_radius && lod >= 0 &&
                lod <= 5 && std::isfinite(lod_number) &&
                lod_number == float(lod) &&
                (index == 0 || lod == prev_lod - 1);
            if (!valid_values || !ordered) {
                JS_FreeValue(context, bands);
                JS_FreeValue(context, streaming);
                return fail(
                    desc, error,
                    "streaming.terrainBands[" + std::to_string(index) + "]",
                    "terrain bands require increasing positive radii and "
                    "consecutive descending LODs in 0..5");
            }
            parsed_bands.push_back({radius, lod});
            prev_radius = radius;
            prev_lod = lod;
        }
        definition.settings.terrain_bands = std::move(parsed_bands);
    }
    JS_FreeValue(context, bands);
    JS_FreeValue(context, streaming);
    return true;
}

// ---------------------------------------------------------------------------
// Legacy fog-multiplier fold (issue 80c66789)
//
// `volumetrics.fogDensityMul / fogFalloffMul / fogFloorOffset` used to be
// applied to the authored FogSettings once per frame in VkVolumetrics::record.
// That made the Lighting panel show every fog concept twice, so the
// multipliers were deleted and this folds the world's authored values through
// the SAME arithmetic the renderer used to do, once, at load.
//
// The two branches below mirror that arithmetic exactly:
//   height layer off -> floor += offset;  falloff *= falloff_mul
//   height layer on  -> min_height += offset;
//                       max_height  = new_min + (old_max - old_min) * falloff_mul
// (the old renderer repurposed fog_floor/fog_falloff as the layer's min/max
// when height_layer was set, which is why the second branch looks nothing like
// the first).
//
// It logs whenever it changes anything. A world author who reads the line can
// paste the printed values back into the script and drop the deprecated keys;
// a world author who does not still gets the picture they had.
void fold_legacy_fog_multipliers(const WorldLoadDesc& desc, FogSettings& fog,
                                 float density_mul, float falloff_mul,
                                 float floor_offset) {
    const bool touched = density_mul != 1.0f || falloff_mul != 1.0f ||
                         floor_offset != 0.0f;
    if (!touched) return;

    const float old_density = fog.density;
    fog.density *= density_mul;

    float old_a = 0.0f, old_b = 0.0f, new_a = 0.0f, new_b = 0.0f;
    const char* name_a = nullptr;
    const char* name_b = nullptr;
    if (fog.height_layer) {
        old_a = fog.min_height;
        old_b = fog.max_height;
        const float depth = (fog.max_height - fog.min_height) * falloff_mul;
        fog.min_height += floor_offset;
        fog.max_height = fog.min_height + depth;
        new_a = fog.min_height;
        new_b = fog.max_height;
        name_a = "minHeight";
        name_b = "maxHeight";
    } else {
        old_a = fog.floor;
        old_b = fog.falloff;
        fog.floor += floor_offset;
        fog.falloff *= falloff_mul;
        new_a = fog.floor;
        new_b = fog.falloff;
        name_a = "floor";
        name_b = "falloff";
    }

    std::fprintf(stderr,
                 "[fog] %s: volumetrics.fogDensityMul/fogFalloffMul/"
                 "fogFloorOffset are deprecated and were folded into "
                 "World.fog: density %g -> %g, %s %g -> %g, %s %g -> %g. "
                 "Paste those values into World.fog and delete the "
                 "volumetrics keys.\n",
                 desc.world_path.c_str(), old_density, fog.density, name_a,
                 old_a, new_a, name_b, old_b, new_b);
}

// Map the legacy single bounded layer (fog.minHeight / maxHeight /
// noiseScale) onto clouds[0], so there is exactly ONE cloud path through the
// renderer and the old spelling is an authoring alias rather than a second
// implementation.
//
// Runs AFTER fold_legacy_fog_multipliers, because that fold rewrites the very
// fields this reads: with the height layer on, the old renderer applied
// fogFloorOffset to minHeight and fogFalloffMul to the layer thickness. Doing
// the mapping first would capture the pre-fold geometry and quietly move the
// deck.
//
// THE PROFILE CHANGES HERE, ON PURPOSE. The old shader gave this layer full
// density everywhere BELOW minHeight and faded it out at maxHeight; a deck now
// exists only between the two (issue 80c66789 part 3). The unbounded column
// beneath the deck cannot be expressed in the new profile and is not meant to
// be — it is what made a second, higher deck impossible. Everything else is
// carried over from the shader code being replaced: density was multiplied by
// 2.4, the fbm ran 3 octaves at lacunarity 2.03 and gain ~0.5, and the
// coverage threshold sat at 0.46..0.57 of a field centred on 0.5, which is
// about 0.55 of the sky filled.
void apply_legacy_height_layer(FogSettings& fog) {
    if (!fog.height_layer) return;
    if (fog.cloud_count > 0) return;  // an explicit fog.clouds array won

    CloudLayer& layer = fog.clouds[0];
    layer = CloudLayer{};
    layer.enabled = true;
    layer.min_height = fog.min_height;
    layer.max_height = fog.max_height;
    layer.max_density = fog.density * 2.4f;
    // The old vertical mask was 1 at the bottom falling to 0 at the top
    // (squared) — all shoulder on the way down and none on the way up. A
    // falloff_max spanning the whole layer reproduces that shape; the base
    // stays hard, as it was.
    layer.falloff_min = 0.0f;
    layer.falloff_max = fog.max_height - fog.min_height;
    layer.noise_scale = fog.noise_scale;
    layer.octaves = 3;
    layer.lacunarity = 2.03f;
    layer.gain = 0.5f;
    layer.coverage = 0.55f;
    for (int i = 0; i < 3; ++i) layer.wind[i] = fog.wind[i];
    sanitize_cloud_layer(layer);
    fog.cloud_count = 1;
    // The legacy layer CONSUMED fog.density as the cloud's density and
    // suppressed the exponential ground term entirely. Zero it, or a world
    // that never had ground fog would grow some.
    fog.density = 0.0f;
}

// World.volumetrics static: editor volumetrics defaults for this world.
// Optional; every field optional with the struct default.
bool extract_volumetrics(JSContext* context,
                         JSValueConst world_class,
                         const WorldLoadDesc& desc,
                         WorldDefinition& definition,
                         WorldLoadError& error) {
    JSValue vol = JS_GetPropertyStr(context, world_class, "volumetrics");
    if (JS_IsUndefined(vol)) {
        JS_FreeValue(context, vol);
        return true;
    }
    if (!JS_IsObject(vol)) {
        JS_FreeValue(context, vol);
        return fail(desc, error, "volumetrics",
                    "World.volumetrics must be an object");
    }
    VulkanVolumetricsSettings& out = definition.settings.volumetrics;
    JSValue enabled = JS_GetPropertyStr(context, vol, "enabled");
    if (!JS_IsUndefined(enabled)) out.enabled = JS_ToBool(context, enabled);
    JS_FreeValue(context, enabled);
    // The three legacy multipliers are read into locals, not into the settings
    // struct — they no longer HAVE a home there (issue 80c66789). Anything a
    // world still authors is folded into the FogSettings field it used to
    // scale, once, at load, with a line on stderr naming both values. A silent
    // change here would be worse than the duplication it replaces: the world
    // would keep looking the same in one build and different in the next with
    // nothing said. extract_fog() runs BEFORE this, so definition.settings.fog
    // already holds the authored values these fold into.
    float legacy_density_mul = 1.0f;
    float legacy_falloff_mul = 1.0f;
    float legacy_floor_offset = 0.0f;
    const struct { const char* name; float* target; } fields[] = {
        {"phaseG", &out.phase_g},
        {"temporalBlend", &out.temporal_blend},
        {"fogDensityMul", &legacy_density_mul},
        {"fogFalloffMul", &legacy_falloff_mul},
        {"fogFloorOffset", &legacy_floor_offset},
    };
    for (const auto& field : fields) {
        JSValue value = JS_GetPropertyStr(context, vol, field.name);
        if (!JS_IsUndefined(value)) {
            float parsed = 0.0f;
            if (!number_value(context, value, parsed) ||
                !std::isfinite(parsed)) {
                JS_FreeValue(context, value);
                JS_FreeValue(context, vol);
                return fail(desc, error,
                            std::string("volumetrics.") + field.name,
                            "must be a finite number");
            }
            *field.target = parsed;
        }
        JS_FreeValue(context, value);
    }
    JS_FreeValue(context, vol);

    fold_legacy_fog_multipliers(desc, definition.settings.fog,
                                legacy_density_mul, legacy_falloff_mul,
                                legacy_floor_offset);
    return true;
}

// ---------------------------------------------------------------------------
// World.props static — script-declared RUNTIME tunables (spec S9).
//
//   static props = {
//     spinSpeed: { default: 1.2, min: 0, max: 10, step: 0.1, doc: "..." },
//     creaky:    { default: true },
//     banner:    { default: "windmill" },
//     season:    { default: 1, enum: ["spring", "summer", "winter"] },
//   };
//
// Kind comes from the `default` literal: number -> Float, boolean -> Bool,
// string -> String, and a number alongside `enum` labels -> Enum (the default
// is then the label INDEX). Int/UInt/Float3/Color3 are not authorable in v1;
// adding them is a matter of extending this switch and WorldPropSpec::Kind.
//
// These are NOT bake inputs. Nothing here reaches a content address or a cache
// key — that is the whole point of the params/props split, and the reason the
// values live in a sidecar props file rather than in the world's hash.
const char* const kPropSpecKeys[] = {
    "default", "min", "max", "step", "doc", "label", "units", "enum",
};

bool prop_spec_string(JSContext* context, JSValueConst spec, const char* key,
                      std::string& output, bool& present) {
    JSValue value = JS_GetPropertyStr(context, spec, key);
    present = !JS_IsUndefined(value);
    const bool ok = !present || string_value(context, value, output);
    JS_FreeValue(context, value);
    return ok;
}

bool prop_spec_finite(JSContext* context, JSValueConst spec, const char* key,
                      float& output, bool& present) {
    JSValue value = JS_GetPropertyStr(context, spec, key);
    present = !JS_IsUndefined(value);
    bool ok = true;
    if (present)
        ok = number_value(context, value, output) && std::isfinite(output);
    JS_FreeValue(context, value);
    return ok;
}

bool extract_prop_spec(JSContext* context,
                       JSValueConst spec,
                       const std::string& path,
                       const WorldLoadDesc& desc,
                       WorldPropSpec& out,
                       WorldLoadError& error) {
    std::string unknown;
    if (!reject_unknown_keys(context, spec, kPropSpecKeys,
                             sizeof(kPropSpecKeys) / sizeof(kPropSpecKeys[0]),
                             unknown)) {
        return fail(desc, error, path,
                    unknown.empty()
                        ? std::string("property spec keys could not be read")
                        : "unknown property spec field '" + unknown + "'");
    }

    // enum first: it decides how a numeric default is read.
    JSValue labels = JS_GetPropertyStr(context, spec, "enum");
    const bool has_enum = !JS_IsUndefined(labels);
    if (has_enum) {
        std::uint32_t count = 0;
        if (!array_length(context, labels, count) || count == 0) {
            JS_FreeValue(context, labels);
            return fail(desc, error, path + ".enum",
                        "enum must be a non-empty array of label strings");
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            JSValue entry = JS_GetPropertyUint32(context, labels, index);
            std::string label;
            const bool ok = string_value(context, entry, label) && !label.empty();
            JS_FreeValue(context, entry);
            if (!ok) {
                JS_FreeValue(context, labels);
                return fail(desc, error,
                            path + ".enum[" + std::to_string(index) + "]",
                            "enum labels must be non-empty strings");
            }
            out.enum_labels.push_back(std::move(label));
        }
    }
    JS_FreeValue(context, labels);

    JSValue fallback = JS_GetPropertyStr(context, spec, "default");
    if (JS_IsUndefined(fallback)) {
        JS_FreeValue(context, fallback);
        return fail(desc, error, path + ".default",
                    "every property must declare a default value");
    }
    bool ok = true;
    if (JS_IsBool(fallback)) {
        if (has_enum) ok = false;
        out.kind = WorldPropSpec::Kind::Bool;
        out.bool_default = JS_ToBool(context, fallback) != 0;
    } else if (JS_IsString(fallback)) {
        if (has_enum) ok = false;
        out.kind = WorldPropSpec::Kind::String;
        ok = ok && string_value(context, fallback, out.string_default);
    } else if (JS_IsNumber(fallback)) {
        double number = 0.0;
        ok = JS_ToFloat64(context, &number, fallback) == 0 && std::isfinite(number);
        out.kind = has_enum ? WorldPropSpec::Kind::Enum : WorldPropSpec::Kind::Float;
        if (ok && has_enum) {
            const double index = std::floor(number);
            if (index != number || index < 0.0 ||
                index >= static_cast<double>(out.enum_labels.size())) {
                JS_FreeValue(context, fallback);
                return fail(desc, error, path + ".default",
                            "an enum default must be an integer index into its "
                            "enum labels");
            }
        }
        out.number_default = number;
    } else {
        ok = false;
    }
    JS_FreeValue(context, fallback);
    if (!ok) {
        return fail(desc, error, path + ".default",
                    has_enum ? "an enum property's default must be a numeric "
                               "label index"
                             : "default must be a finite number, a boolean or a "
                               "string");
    }

    bool present = false;
    if (!prop_spec_string(context, spec, "label", out.label, present) ||
        !prop_spec_string(context, spec, "doc", out.doc, present) ||
        !prop_spec_string(context, spec, "units", out.units, present)) {
        return fail(desc, error, path,
                    "label, doc and units must be strings when present");
    }

    bool has_min = false, has_max = false, has_step = false;
    if (!prop_spec_finite(context, spec, "min", out.min, has_min) ||
        !prop_spec_finite(context, spec, "max", out.max, has_max) ||
        !prop_spec_finite(context, spec, "step", out.step, has_step)) {
        return fail(desc, error, path, "min, max and step must be finite numbers");
    }
    if (has_min != has_max) {
        return fail(desc, error, path,
                    "min and max must be declared together (a half-open range "
                    "has no slider)");
    }
    if (has_min && out.min > out.max)
        return fail(desc, error, path, "min must not exceed max");
    out.has_range = has_min;
    // A range only means anything for the numeric kinds; keeping it on a Bool
    // or a String would clamp nothing and mislead the panel.
    if (out.kind != WorldPropSpec::Kind::Float) {
        out.has_range = false;
        out.step = 0.0f;
    }
    if (out.has_range &&
        (out.number_default < out.min || out.number_default > out.max)) {
        return fail(desc, error, path + ".default",
                    "default lies outside the declared min/max range");
    }
    return true;
}

bool extract_props(JSContext* context,
                   JSValueConst world_class,
                   const WorldLoadDesc& desc,
                   WorldDefinition& definition,
                   WorldLoadError& error) {
    JSValue props = JS_GetPropertyStr(context, world_class, "props");
    if (JS_IsUndefined(props)) {
        JS_FreeValue(context, props);
        return true;
    }
    if (!JS_IsObject(props) || JS_IsArray(props)) {
        JS_FreeValue(context, props);
        return fail(desc, error, "props",
                    "World.props must be an object of name -> property spec");
    }

    JSPropertyEnum* names = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &names, &count, props,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        JS_FreeValue(context, props);
        return fail(desc, error, "props", "World.props keys could not be read");
    }

    bool ok = true;
    for (std::uint32_t index = 0; index < count && ok; ++index) {
        const char* text = JS_AtomToCString(context, names[index].atom);
        if (!text) {
            ok = fail(desc, error, "props", "World.props key is not a string");
            break;
        }
        WorldPropSpec spec;
        spec.name = text;
        JS_FreeCString(context, text);
        const std::string path = "props." + spec.name;

        for (const WorldPropSpec& existing : definition.props) {
            if (existing.name == spec.name) {
                ok = fail(desc, error, path, "duplicate property name");
                break;
            }
        }
        if (!ok) break;

        JSValue entry = JS_GetProperty(context, props, names[index].atom);
        if (!JS_IsObject(entry) || JS_IsArray(entry)) {
            JS_FreeValue(context, entry);
            ok = fail(desc, error, path,
                      "a property spec must be an object, e.g. { default: 1 }");
            break;
        }
        ok = extract_prop_spec(context, entry, path, desc, spec, error);
        JS_FreeValue(context, entry);
        if (ok) definition.props.push_back(std::move(spec));
    }

    JS_FreePropertyEnum(context, names, count);
    JS_FreeValue(context, props);
    if (!ok) definition.props.clear();
    return ok;
}

// getProp(name) — the world script's read side of its own `static props`.
//
// DEFINITION-TIME ONLY, and it returns the DECLARED DEFAULT. There is no
// long-lived JS context in this engine (every runtime is created and freed
// inside one load/bake call), so there is nothing for a live value to flow
// into; and wiring the editor's override in here would make the world's
// GEOMETRY depend on a value that is deliberately not in any cache key, which
// is how you get a stale bake that nothing invalidates. See the Phase-6 seam
// note in the property-system spec.
JSValue get_prop(JSContext* context,
                 JSValueConst,
                 int argument_count,
                 JSValueConst* arguments) {
    LoadCollector* collector =
        static_cast<LoadCollector*>(JS_GetContextOpaque(context));
    if (!collector || !collector->props)
        return JS_ThrowInternalError(context, "property collector unavailable");
    std::string name;
    if (argument_count < 1 || !string_value(context, arguments[0], name) ||
        name.empty()) {
        return JS_ThrowTypeError(context,
                                 "getProp(name): name must be a non-empty string");
    }
    for (const WorldPropSpec& spec : *collector->props) {
        if (spec.name != name) continue;
        switch (spec.kind) {
            case WorldPropSpec::Kind::Bool:
                return JS_NewBool(context, spec.bool_default ? 1 : 0);
            case WorldPropSpec::Kind::String:
                return JS_NewStringLen(context, spec.string_default.c_str(),
                                       spec.string_default.size());
            case WorldPropSpec::Kind::Enum: {
                // The LABEL, not the index: a script branches on the name it
                // authored, and parse_and_set accepts either form on the way
                // back in.
                const std::size_t index =
                    static_cast<std::size_t>(spec.number_default);
                if (index >= spec.enum_labels.size())
                    return JS_ThrowInternalError(context,
                                                 "getProp('%s'): enum index out of range",
                                                 name.c_str());
                const std::string& label = spec.enum_labels[index];
                return JS_NewStringLen(context, label.c_str(), label.size());
            }
            case WorldPropSpec::Kind::Float:
                return JS_NewFloat64(context, spec.number_default);
        }
    }
    return JS_ThrowReferenceError(
        context, "getProp('%s'): no such entry in this World's static props",
        name.c_str());
}

JSValue get_prop_too_early(JSContext* context, JSValueConst, int, JSValueConst*) {
    return JS_ThrowTypeError(
        context,
        "getProp is not available while the class statics evaluate — the "
        "`static props` block is itself one of them. Call it from "
        "buildEntities()");
}

bool extract_settings(JSContext* context,
                      JSValueConst world_class,
                      const WorldLoadDesc& desc,
                      WorldDefinition& definition,
                      WorldLoadError& error) {
    for (const char* property : {"world", "settings"}) {
        JSValue settings = JS_GetPropertyStr(context, world_class, property);
        if (!JS_IsUndefined(settings) &&
            (!JS_IsObject(settings) ||
             !extract_settings_object(context, settings, definition.settings))) {
            JS_FreeValue(context, settings);
            return fail(desc, error, property,
                        std::string("World.") + property +
                            " must contain numeric sectorSize/yMin/yMax values");
        }
        JS_FreeValue(context, settings);
    }
    return true;
}

bool append_static_entities(JSContext* context,
                            JSValueConst world_class,
                            const WorldLoadDesc& desc,
                            WorldLoadError& error) {
    JSValue entities = JS_GetPropertyStr(context, world_class, "entities");
    if (JS_IsUndefined(entities)) {
        JS_FreeValue(context, entities);
        return true;
    }
    std::uint32_t count = 0;
    if (!array_length(context, entities, count)) {
        JS_FreeValue(context, entities);
        return fail(desc, error, "entities", "World.entities must be an array");
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue combined = JS_GetPropertyStr(context, global, "__matter_entities");
    JS_FreeValue(context, global);
    for (std::uint32_t index = 0; index < count; ++index) {
        JSValue entry = JS_GetPropertyUint32(context, entities, index);
        JS_SetPropertyUint32(context, combined, index, entry);
    }
    JS_FreeValue(context, combined);
    JS_FreeValue(context, entities);
    return true;
}

bool extract_entities(JSContext* context,
                      JSValueConst canonicalizer,
                      const WorldLoadDesc& desc,
                      WorldDefinition& definition,
                      WorldLoadError& error) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue entities = JS_GetPropertyStr(context, global, "__matter_entities");
    JS_FreeValue(context, global);
    std::uint32_t count = 0;
    if (!array_length(context, entities, count)) {
        JS_FreeValue(context, entities);
        return fail(desc, error, "entities", "entity collection is not an array");
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::string path = "entities[" + std::to_string(index) + "]";
        JSValue entry = JS_GetPropertyUint32(context, entities, index);
        RawEntityRecipe recipe;
        JSValue id = JS_GetPropertyStr(context, entry, "id");
        if (!string_value(context, id, recipe.authored_id)) {
            JS_FreeValue(context, id);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, entities);
            return fail(desc, error, path + ".id", "entity id must be a string");
        }
        JS_FreeValue(context, id);
        JSValue name = JS_GetPropertyStr(context, entry, "name");
        if (JS_IsUndefined(name)) {
            recipe.display_name = recipe.authored_id;
        } else if (!string_value(context, name, recipe.display_name)) {
            JS_FreeValue(context, name);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, entities);
            return fail(desc, error, path + ".name", "entity name must be a string");
        }
        JS_FreeValue(context, name);
        JSValue parent = JS_GetPropertyStr(context, entry, "parent");
        if (!JS_IsUndefined(parent) &&
            !string_value(context, parent, recipe.parent_authored_id)) {
            JS_FreeValue(context, parent);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, entities);
            return fail(desc, error, path + ".parent",
                        "entity parent must be a string");
        }
        JS_FreeValue(context, parent);
        const bool components_present = has_property(context, entry, "components");
        JSValue components = JS_GetPropertyStr(context, entry, "components");
        if (!components_present) {
            JS_FreeValue(context, components);
            components = JS_NewObject(context);
        }
        if (!canonical_json(context, canonicalizer, components,
                            recipe.components_json)) {
            JS_FreeValue(context, components);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, entities);
            return fail(desc, error, path + ".components",
                        "entity components must be JSON serializable");
        }
        JS_FreeValue(context, components);
        JS_FreeValue(context, entry);
        definition.entities.push_back(std::move(recipe));
    }
    JS_FreeValue(context, entities);
    return true;
}

} // namespace

bool load_world_definition(const WorldLoadDesc& desc,
                           WorldDefinition& definition,
                           WorldLoadError& error) {
    definition = WorldDefinition{};
    error = WorldLoadError{};

    // Contract C3: the dynamic registry tail is per-world. Clearing it here (not
    // only at provider connect) is what makes handles deterministic — loading
    // the same world twice yields the same indices, and a second world never
    // inherits the first world's materials. Builtin ids 0..29 are untouched.
    MaterialRegistryResetDynamic();

    std::string source;
    if (!read_text_file(desc.world_path, source)) {
        return fail(desc, error, "source", "unable to read world source");
    }
    const std::string class_name =
        world_script_detail::find_world_class_name(source);
    if (class_name.empty()) {
        return fail(desc, error, "class", "no class extending World found");
    }

    ModuleStore modules;
    std::string module_error;
    if (!gather_modules(source, desc, modules, module_error)) {
        return fail(desc, error, "imports", std::move(module_error));
    }
    const bool use_modules = !modules.sources.empty();
    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) return fail(desc, error, "runtime", "unable to create JavaScript runtime");
    if (use_modules) {
        JS_SetModuleLoaderFunc(runtime, normalize_module, load_module, &modules);
    }
    JSContext* context = new_world_context(runtime, use_modules);
    if (!context) {
        JS_FreeRuntime(runtime);
        return fail(desc, error, "runtime", "unable to create JavaScript context");
    }
    auto cleanup = [&]() {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
    };

    static constexpr const char* base_source = R"JS(
globalThis.__matter_entities = [];
Math.random = undefined;
class World {}
)JS";
    JSValue base = JS_Eval(context, base_source, std::strlen(base_source),
                           "<world-definition-base>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(base)) {
        const std::string message = exception_message(context);
        JS_FreeValue(context, base);
        cleanup();
        return fail(desc, error, "runtime", message);
    }
    JS_FreeValue(context, base);

    JSValue params = JS_ParseJSON(context, desc.canonical_params_json.c_str(),
                                  desc.canonical_params_json.size(),
                                  "<world-params>");
    if (JS_IsException(params)) {
        const std::string message = exception_message(context);
        JS_FreeValue(context, params);
        cleanup();
        return fail(desc, error, "params", message);
    }
    JSValue global = JS_GetGlobalObject(context);
    JS_SetPropertyStr(context, global, "__matter_params", params);
    JS_SetPropertyStr(context, global, "__matter_world_seed",
                      JS_NewInt64(context, static_cast<std::int64_t>(desc.world_seed)));
    // defineMaterial must exist before the world module evaluates: materials are
    // declared at module scope / in class statics, and roots may reference the
    // returned handles. The collector lives on the stack for this whole call and
    // rides the context opaque (this context has no other opaque owner).
    LoadCollector load_collector;
    load_collector.materials = &definition.materials;
    load_collector.props = &definition.props;
    JS_SetContextOpaque(context, &load_collector);
    JS_SetPropertyStr(context, global, "defineMaterial",
                      JS_NewCFunction(context, define_material, "defineMaterial", 2));
    // getProp exists from the start so a misuse names itself; it only becomes
    // readable once `static props` has been extracted, below.
    JS_SetPropertyStr(context, global, "getProp",
                      JS_NewCFunction(context, get_prop_too_early, "getProp", 1));
    JS_FreeValue(context, global);

    const std::string wrapped = source +
        "\n;globalThis.__matter_world_class = " + class_name + ";\n";
    JSValue evaluated = JS_Eval(context, wrapped.c_str(), wrapped.size(),
                                desc.world_path.c_str(),
                                use_modules ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(evaluated) ||
        (use_modules && !execute_jobs(runtime, context)) ||
        (use_modules && JS_IsObject(evaluated) &&
         JS_PromiseState(context, evaluated) == JS_PROMISE_REJECTED)) {
        if (use_modules && JS_IsObject(evaluated) &&
            JS_PromiseState(context, evaluated) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(context, evaluated);
            JS_Throw(context, reason);
        }
        const std::string message = exception_message(context);
        JS_FreeValue(context, evaluated);
        cleanup();
        return fail(desc, error, "source", message);
    }
    JS_FreeValue(context, evaluated);

    global = JS_GetGlobalObject(context);
    JSValue world_class =
        JS_GetPropertyStr(context, global, "__matter_world_class");
    JS_FreeValue(context, global);
    if (!JS_IsFunction(context, world_class)) {
        JS_FreeValue(context, world_class);
        cleanup();
        return fail(desc, error, "class", "World class is not a constructor");
    }

    static constexpr const char* canonicalizer_source = R"JS(
(function(value) {
  function normalize(item) {
    if (Array.isArray(item)) return item.map(normalize);
    if (item && typeof item === 'object') {
      const result = {};
      for (const key of Object.keys(item).sort()) result[key] = normalize(item[key]);
      return result;
    }
    return item;
  }
  return JSON.stringify(normalize(value));
})
)JS";
    JSValue canonicalizer =
        JS_Eval(context, canonicalizer_source, std::strlen(canonicalizer_source),
                "<canonical-json>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(canonicalizer)) {
        const std::string message = exception_message(context);
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return fail(desc, error, "runtime", message);
    }

    bool ok = extract_roots(context, world_class, canonicalizer, desc,
                            definition, error) &&
              extract_settings(context, world_class, desc, definition, error) &&
              extract_lights(context, world_class, desc, definition, error) &&
              extract_fog(context, world_class, desc, definition, error) &&
              extract_camera(context, world_class, desc, definition, error) &&
              extract_streaming(context, world_class, desc, definition, error) &&
              extract_volumetrics(context, world_class, desc, definition,
                                  error) &&
              extract_props(context, world_class, desc, definition, error) &&
              append_static_entities(context, world_class, desc, error);
    // The legacy bounded layer becomes clouds[0] here, not inside
    // extract_fog: it has to run after extract_volumetrics' multiplier fold
    // has finished rewriting minHeight/maxHeight/density, and extract_fog
    // runs before that. Unconditional rather than folded into the chain above
    // because a world can author `fog.minHeight` with no `volumetrics` block
    // at all, and that world still needs its deck.
    if (ok) apply_legacy_height_layer(definition.settings.fog);
    if (!ok) {
        definition = WorldDefinition{};
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return false;
    }

    // Roots are read; anything declaring a material from here on (buildEntities)
    // is too late for the provider's detail-bake scheduling, so say so loudly
    // instead of accepting a material nothing will ever bake.
    {
        JSValue globals = JS_GetGlobalObject(context);
        JS_SetPropertyStr(
            context, globals, "defineMaterial",
            JS_NewCFunction(context, define_material_too_late, "defineMaterial", 2));
        // The mirror image: `static props` is now parsed, so buildEntities()
        // can read its declared defaults.
        JS_SetPropertyStr(context, globals, "getProp",
                          JS_NewCFunction(context, get_prop, "getProp", 1));
        JS_FreeValue(context, globals);
    }

    // Bypass an authored constructor: the contract evaluates class statics and
    // optional buildEntities() only.
    JSValue prototype = JS_GetPropertyStr(context, world_class, "prototype");
    JSValue instance = JS_NewObjectProto(context, prototype);
    JS_FreeValue(context, prototype);
    global = JS_GetGlobalObject(context);
    JSValue bound_params = JS_GetPropertyStr(context, global, "__matter_params");
    JSValue bound_seed = JS_GetPropertyStr(context, global, "__matter_world_seed");
    JS_FreeValue(context, global);
    JS_SetPropertyStr(context, instance, "params", bound_params);
    JS_SetPropertyStr(context, instance, "worldSeed", bound_seed);
    // Own, non-writable and non-configurable: authored prototypes or instance
    // assignment cannot intercept/suppress the bootstrap append operation.
    JS_DefinePropertyValueStr(
        context, instance, "entity",
        JS_NewCFunction(context, append_entity, "entity", 1), 0);

    JSValue build = JS_GetPropertyStr(context, instance, "buildEntities");
    if (JS_IsException(build)) {
        const std::string message = exception_message(context);
        definition = WorldDefinition{};
        JS_FreeValue(context, build);
        JS_FreeValue(context, instance);
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return fail(desc, error, "buildEntities", message);
    }
    if (!JS_IsUndefined(build)) {
        if (!JS_IsFunction(context, build)) {
            definition = WorldDefinition{};
            JS_FreeValue(context, build);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "buildEntities",
                        "World.buildEntities must be a function");
        }
        JSValue result = JS_Call(context, build, instance, 0, nullptr);
        if (JS_IsException(result)) {
            const std::string message = exception_message(context);
            definition = WorldDefinition{};
            JS_FreeValue(context, result);
            JS_FreeValue(context, build);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "buildEntities", message);
        }
        JS_FreeValue(context, result);
    }
    JS_FreeValue(context, build);
    JS_FreeValue(context, instance);

    ok = extract_entities(context, canonicalizer, desc, definition, error);
    JS_FreeValue(context, canonicalizer);
    JS_FreeValue(context, world_class);
    cleanup();
    if (!ok) definition = WorldDefinition{};
    return ok;
}

} // namespace matter
