#include "world_definition_loader.h"

#include "module_resolver.h"

extern "C" {
#include "quickjs.h"
}

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
    const char* text = JS_IsString(stack) ? JS_ToCString(context, stack) : nullptr;
    if (!text) text = JS_ToCString(context, exception);
    std::string result = text ? text : "JavaScript exception";
    if (text) JS_FreeCString(context, text);
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
    const char* text = JS_ToCString(context, result);
    if (text) output = text;
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, result);
    return text != nullptr;
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

        JSValue params = JS_GetPropertyStr(context, entry, "params");
        if (!JS_IsUndefined(params) &&
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
        JSValue direction = JS_GetPropertyStr(context, sun, "dir");
        JSValue color = JS_GetPropertyStr(context, sun, "color");
        const bool ok = float3_value(context, direction, definition.settings.sun_direction) &&
                        float3_value(context, color, definition.settings.sun_color);
        JS_FreeValue(context, direction);
        JS_FreeValue(context, color);
        if (!ok) {
            JS_FreeValue(context, sun);
            JS_FreeValue(context, lights);
            return fail(desc, error, "lights.sun",
                        "sun.dir and sun.color must contain 3 numbers");
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
        JSValue components = JS_GetPropertyStr(context, entry, "components");
        if (JS_IsUndefined(components)) {
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
class World {
  entity(record) { globalThis.__matter_entities.push(record); }
}
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
              append_static_entities(context, world_class, desc, error);
    if (!ok) {
        definition = WorldDefinition{};
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return false;
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

    JSValue build = JS_GetPropertyStr(context, instance, "buildEntities");
    if (!JS_IsUndefined(build)) {
        if (!JS_IsFunction(context, build)) {
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
