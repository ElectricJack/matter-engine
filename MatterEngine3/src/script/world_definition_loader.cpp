#include "world_definition_loader.h"
#include "matter/log.h"

#include "../hydrology/hydrology_settings.h"
#include "../hydrology/river_network_builder.h"
#include "../terrain_collision/terrain_collision_definition.h"
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
#include <memory>
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
    std::unique_ptr<hydrology::RiverNetworkBuilder> river_builder;
    std::optional<RiverNetworkDefinition> finished_river_network;
    std::string river_error_path;
    bool river_network_created = false;
    bool river_network_built = false;
    bool river_hydrology_active = false;
    TerrainCollisionDefinition terrain_collision_settings{};
    std::vector<TerrainCollisionRegion> terrain_collision_regions;
    std::set<std::string> terrain_collision_ids;
    std::string terrain_collision_error_path;
    std::string terrain_collision_phase = "module scope";
    float terrain_collision_sector_size_m = 0.0f;
    bool terrain_collision_active = false;
    bool terrain_collision_created = false;
    bool terrain_collision_built = false;
};

struct RiverNetworkHandle {
    LoadCollector* collector = nullptr;
};

struct RiverHandle {
    LoadCollector* collector = nullptr;
    std::size_t river = 0;
};

struct RiverSectionHandle {
    LoadCollector* collector = nullptr;
    std::size_t section = 0;
};

JSClassID river_network_class_id = 0;
JSClassID river_class_id = 0;
JSClassID river_section_class_id = 0;

void river_network_finalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<RiverNetworkHandle*>(
        JS_GetOpaque(value, river_network_class_id));
}

void river_finalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<RiverHandle*>(JS_GetOpaque(value, river_class_id));
}

void river_section_finalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<RiverSectionHandle*>(
        JS_GetOpaque(value, river_section_class_id));
}

bool install_river_classes(JSRuntime* runtime) {
    if (river_network_class_id == 0)
        JS_NewClassID(runtime, &river_network_class_id);
    if (river_class_id == 0) JS_NewClassID(runtime, &river_class_id);
    if (river_section_class_id == 0)
        JS_NewClassID(runtime, &river_section_class_id);
    const JSClassDef network_class = {
        "MatterRiverNetwork", river_network_finalizer, nullptr, nullptr, nullptr};
    const JSClassDef river_class = {
        "MatterRiver", river_finalizer, nullptr, nullptr, nullptr};
    const JSClassDef section_class = {
        "MatterRiverSection", river_section_finalizer, nullptr, nullptr, nullptr};
    return (JS_IsRegisteredClass(runtime, river_network_class_id) ||
            JS_NewClass(runtime, river_network_class_id, &network_class) == 0) &&
           (JS_IsRegisteredClass(runtime, river_class_id) ||
            JS_NewClass(runtime, river_class_id, &river_class) == 0) &&
           (JS_IsRegisteredClass(runtime, river_section_class_id) ||
            JS_NewClass(runtime, river_section_class_id, &section_class) == 0);
}

std::string error_path(const std::string& error) {
    const std::size_t separator = error.find(':');
    return separator == std::string::npos ? "hydrology" : error.substr(0, separator);
}

JSValue river_failure(JSContext* context, LoadCollector* collector,
                      const std::string& error) {
    if (collector) collector->river_error_path = error_path(error);
    return JS_ThrowTypeError(context, "%s", error.c_str());
}

JSValue river_phase_failure(JSContext* context, LoadCollector* collector) {
    return river_failure(
        context, collector,
        "hydrology.phase: river builders are only available inside hydrology()");
}

JSValue terrain_collision_failure(JSContext* context, LoadCollector* collector,
                                  const std::string& path,
                                  const std::string& message) {
    if (collector) collector->terrain_collision_error_path = path;
    return JS_ThrowTypeError(context, "%s", message.c_str());
}

JSValue terrain_collision_phase_failure(JSContext* context, LoadCollector* collector) {
    const std::string phase = collector ? collector->terrain_collision_phase : "unknown";
    return terrain_collision_failure(
        context, collector, phase,
        "terrainCollision() is only available inside collision(); active phase is " + phase);
}

bool terrain_collision_float3(JSContext* context, JSValueConst value, Float3& output) {
    if (!float3_value(context, value, output)) return false;
    return std::isfinite(output.x) && std::isfinite(output.y) && std::isfinite(output.z);
}

JSValue terrain_collision_region(JSContext* context, JSValueConst,
                                 int argument_count, JSValueConst* arguments) {
    LoadCollector* collector = static_cast<LoadCollector*>(JS_GetContextOpaque(context));
    if (!collector || !collector->terrain_collision_active)
        return terrain_collision_phase_failure(context, collector);
    if (collector->terrain_collision_built)
        return terrain_collision_failure(context, collector, "terrainCollision.region",
                                         "terrainCollision.region() cannot follow build()");
    std::string id;
    if (argument_count < 2 || !string_value(context, arguments[0], id) || id.empty()) {
        return terrain_collision_failure(context, collector, "terrainCollision.region.id",
                                         "terrainCollision.region(id, bounds) requires a non-empty id");
    }
    if (!collector->terrain_collision_ids.insert(id).second) {
        return terrain_collision_failure(context, collector, "terrainCollision.region[" + id + "].id",
                                         "terrainCollision.region ids must be unique");
    }
    if (!JS_IsObject(arguments[1]) || JS_IsArray(arguments[1])) {
        return terrain_collision_failure(context, collector, "terrainCollision.region[" + id + "]",
                                         "terrainCollision.region(id, bounds) requires a bounds object");
    }
    Float3 min_m{};
    Float3 max_m{};
    JSValue min_value = JS_GetPropertyStr(context, arguments[1], "min");
    const bool min_ok = terrain_collision_float3(context, min_value, min_m);
    JS_FreeValue(context, min_value);
    if (!min_ok) {
        return terrain_collision_failure(context, collector, "terrainCollision.region[" + id + "].min",
                                         "terrainCollision.region[" + id + "].min must be three finite numbers");
    }
    JSValue max_value = JS_GetPropertyStr(context, arguments[1], "max");
    const bool max_ok = terrain_collision_float3(context, max_value, max_m);
    JS_FreeValue(context, max_value);
    if (!max_ok) {
        return terrain_collision_failure(context, collector, "terrainCollision.region[" + id + "].max",
                                         "terrainCollision.region[" + id + "].max must be three finite numbers");
    }
    collector->terrain_collision_regions.push_back({std::move(id), min_m, max_m});
    return JS_UNDEFINED;
}

JSValue terrain_collision_build(JSContext* context, JSValueConst,
                                int, JSValueConst*) {
    LoadCollector* collector = static_cast<LoadCollector*>(JS_GetContextOpaque(context));
    if (!collector || !collector->terrain_collision_active)
        return terrain_collision_phase_failure(context, collector);
    if (collector->terrain_collision_built)
        return terrain_collision_failure(context, collector, "terrainCollision.build",
                                         "terrainCollision.build() may only be called once");
    if (collector->terrain_collision_regions.empty())
        return terrain_collision_failure(context, collector, "terrainCollision.build",
                                         "terrainCollision.build() requires at least one region");
    collector->terrain_collision_settings.regions = collector->terrain_collision_regions;
    terrain_collision::CanonicalDefinition canonical;
    std::string validation_error;
    if (!terrain_collision::canonicalize(collector->terrain_collision_settings,
                                         collector->terrain_collision_sector_size_m,
                                         {}, canonical, validation_error)) {
        const std::size_t separator = validation_error.find(' ');
        const std::string path = separator == std::string::npos
            ? "terrainCollision.build" : validation_error.substr(0, separator);
        return terrain_collision_failure(context, collector, path, validation_error);
    }
    collector->terrain_collision_built = true;
    return JS_UNDEFINED;
}

JSValue terrain_collision_builder(JSContext* context, JSValueConst,
                                  int argument_count, JSValueConst* arguments) {
    LoadCollector* collector = static_cast<LoadCollector*>(JS_GetContextOpaque(context));
    if (!collector || !collector->terrain_collision_active)
        return terrain_collision_phase_failure(context, collector);
    if (collector->terrain_collision_created)
        return terrain_collision_failure(context, collector, "terrainCollision",
                                         "terrainCollision() may create only one builder");
    if (argument_count < 1 || !JS_IsObject(arguments[0]) || JS_IsArray(arguments[0])) {
        return terrain_collision_failure(context, collector, "terrainCollision",
                                         "terrainCollision(options) requires an options object");
    }
    float cell_size_m = 0.0f;
    JSValue cell_size = JS_GetPropertyStr(context, arguments[0], "cellSize");
    const bool cell_ok = number_value(context, cell_size, cell_size_m) &&
                         std::isfinite(cell_size_m);
    JS_FreeValue(context, cell_size);
    std::int8_t rung = 0;
    if (!cell_ok || !terrain_collision::cell_size_to_rung(cell_size_m, rung)) {
        return terrain_collision_failure(context, collector, "terrainCollision.cellSize",
                                         "terrainCollision.cellSize must be one of the supported terrain rungs");
    }
    float friction = 0.7f;
    float restitution = 0.0f;
    JSValue friction_value = JS_GetPropertyStr(context, arguments[0], "friction");
    const bool friction_ok = JS_IsUndefined(friction_value) ||
                             (number_value(context, friction_value, friction) && std::isfinite(friction));
    JS_FreeValue(context, friction_value);
    JSValue restitution_value = JS_GetPropertyStr(context, arguments[0], "restitution");
    const bool restitution_ok = JS_IsUndefined(restitution_value) ||
                                (number_value(context, restitution_value, restitution) && std::isfinite(restitution));
    JS_FreeValue(context, restitution_value);
    if (!friction_ok || friction < 0.0f || friction > 1.0f) {
        return terrain_collision_failure(context, collector, "terrainCollision.friction",
                                         "terrainCollision.friction must be finite and in [0, 1]");
    }
    if (!restitution_ok || restitution < 0.0f || restitution > 1.0f) {
        return terrain_collision_failure(context, collector, "terrainCollision.restitution",
                                         "terrainCollision.restitution must be finite and in [0, 1]");
    }
    collector->terrain_collision_settings = {};
    collector->terrain_collision_settings.cell_size_m = cell_size_m;
    collector->terrain_collision_settings.rung = rung;
    collector->terrain_collision_settings.friction = friction;
    collector->terrain_collision_settings.restitution = restitution;
    collector->terrain_collision_created = true;
    JSValue object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "region",
                      JS_NewCFunction(context, terrain_collision_region, "region", 2));
    JS_SetPropertyStr(context, object, "build",
                      JS_NewCFunction(context, terrain_collision_build, "build", 0));
    return object;
}

bool required_float(JSContext* context, JSValueConst object, const char* key,
                    float& output) {
    JSValue value = JS_GetPropertyStr(context, object, key);
    double number = 0.0;
    const bool ok = JS_IsNumber(value) &&
                    JS_ToFloat64(context, &number, value) == 0 &&
                    number >= -std::numeric_limits<float>::max() &&
                    number <= std::numeric_limits<float>::max();
    if (ok) output = static_cast<float>(number);
    JS_FreeValue(context, value);
    return ok;
}

bool required_uint32(JSContext* context, JSValueConst object, const char* key,
                     std::uint32_t& output) {
    JSValue value = JS_GetPropertyStr(context, object, key);
    double number = 0.0;
    const bool ok = JS_IsNumber(value) &&
                    JS_ToFloat64(context, &number, value) == 0 &&
                    std::isfinite(number) && number >= 0.0 &&
                    std::floor(number) == number &&
                    number <= std::numeric_limits<std::uint32_t>::max();
    if (ok) output = static_cast<std::uint32_t>(number);
    JS_FreeValue(context, value);
    return ok;
}

JSValue river_inlet(JSContext* context, JSValueConst this_value,
                    int argument_count, JSValueConst* arguments) {
    RiverHandle* handle = static_cast<RiverHandle*>(
        JS_GetOpaque2(context, this_value, river_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    RiverInlet inlet{};
    if (argument_count < 2 ||
        !float3_value(context, arguments[0], inlet.position_m) ||
        !JS_IsObject(arguments[1]) ||
        !required_float(context, arguments[1], "flow", inlet.flow_m3s)) {
        return river_failure(context, handle->collector,
                             "hydrology.inlet: inlet(position, {flow}) requires finite values");
    }
    std::string error;
    if (!handle->collector->river_builder->set_inlet(handle->river, inlet, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue river_curve(JSContext* context, JSValueConst this_value,
                    int argument_count, JSValueConst* arguments) {
    RiverHandle* handle = static_cast<RiverHandle*>(
        JS_GetOpaque2(context, this_value, river_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    std::uint32_t count = 0;
    std::vector<Float3> curve;
    if (argument_count < 1 || !array_length(context, arguments[0], count))
        return river_failure(context, handle->collector,
                             "hydrology.curve: curve(points) requires an array");
    curve.reserve(count);
    for (std::uint32_t point = 0; point < count; ++point) {
        JSValue value = JS_GetPropertyUint32(context, arguments[0], point);
        Float3 position{};
        const bool ok = float3_value(context, value, position);
        JS_FreeValue(context, value);
        if (!ok)
            return river_failure(context, handle->collector,
                                 "hydrology.curve: every point must contain three numbers");
        curve.push_back(position);
    }
    std::string error;
    if (!handle->collector->river_builder->set_curve(handle->river, curve, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue river_channel_profile(JSContext* context, JSValueConst this_value,
                              int argument_count, JSValueConst* arguments) {
    RiverHandle* handle = static_cast<RiverHandle*>(
        JS_GetOpaque2(context, this_value, river_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    std::uint32_t count = 0;
    std::vector<RiverChannelProfilePoint> profile;
    if (argument_count < 1 || !array_length(context, arguments[0], count))
        return river_failure(
            context, handle->collector,
            "hydrology.channelProfile: channelProfile(points) requires an array");
    profile.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        JSValue value = JS_GetPropertyUint32(context, arguments[0], index);
        RiverChannelProfilePoint point{};
        const bool ok = JS_IsObject(value) &&
                        required_float(context, value, "at", point.distance_m) &&
                        required_float(context, value, "width", point.width_m) &&
                        required_float(context, value, "depth", point.depth_m) &&
                        required_float(context, value, "asymmetry", point.asymmetry);
        JS_FreeValue(context, value);
        if (!ok)
            return river_failure(
                context, handle->collector,
                "hydrology.channelProfile: every point requires at/width/depth/asymmetry");
        profile.push_back(point);
    }
    std::string error;
    if (!handle->collector->river_builder->set_channel_profile(
            handle->river, profile, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue river_joins(JSContext* context, JSValueConst this_value,
                    int, JSValueConst*) {
    RiverHandle* handle = static_cast<RiverHandle*>(
        JS_GetOpaque2(context, this_value, river_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    std::string error;
    handle->collector->river_builder->reserve_join(handle->river, error);
    return river_failure(context, handle->collector, error);
}

JSValue river_section(JSContext* context, JSValueConst this_value,
                      int argument_count, JSValueConst* arguments);

JSValue make_river_object(JSContext* context, LoadCollector* collector,
                          std::size_t river) {
    JSValue object = JS_NewObjectClass(context, river_class_id);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new RiverHandle{collector, river});
    JS_SetPropertyStr(context, object, "inlet",
                      JS_NewCFunction(context, river_inlet, "inlet", 2));
    JS_SetPropertyStr(context, object, "curve",
                      JS_NewCFunction(context, river_curve, "curve", 1));
    JS_SetPropertyStr(
        context, object, "channelProfile",
        JS_NewCFunction(context, river_channel_profile, "channelProfile", 1));
    JS_SetPropertyStr(context, object, "section",
                      JS_NewCFunction(context, river_section, "section", 2));
    JS_SetPropertyStr(context, object, "joins",
                      JS_NewCFunction(context, river_joins, "joins", 1));
    return object;
}

JSValue network_river(JSContext* context, JSValueConst this_value,
                      int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = static_cast<RiverNetworkHandle*>(
        JS_GetOpaque2(context, this_value, river_network_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    std::string name;
    if (argument_count < 1 || !string_value(context, arguments[0], name))
        return river_failure(context, handle->collector,
                             "hydrology.river.name: river(name) requires a string");
    std::size_t river = 0;
    std::string error;
    if (!handle->collector->river_builder->add_river(name, river, error))
        return river_failure(context, handle->collector, error);
    return make_river_object(context, handle->collector, river);
}

RiverSectionHandle* active_section_handle(JSContext* context,
                                          JSValueConst this_value) {
    auto* handle = static_cast<RiverSectionHandle*>(
        JS_GetOpaque2(context, this_value, river_section_class_id));
    if (!handle) return nullptr;
    if (!handle->collector->river_hydrology_active) {
        river_phase_failure(context, handle->collector);
        return nullptr;
    }
    return handle;
}

JSValue section_emitters(JSContext* context, JSValueConst this_value,
                         int argument_count, JSValueConst* arguments) {
    RiverSectionHandle* handle = active_section_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    std::uint32_t count = 0;
    if (argument_count < 1 || !array_length(context, arguments[0], count))
        return river_failure(context, handle->collector,
                             "hydrology.section.emitters: emitters requires an array of ids");
    std::vector<std::string> ids;
    ids.reserve(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        JSValue value = JS_GetPropertyUint32(context, arguments[0], index);
        std::string id;
        const bool ok = string_value(context, value, id);
        JS_FreeValue(context, value);
        if (!ok)
            return river_failure(context, handle->collector,
                                 "hydrology.section.emitters: every emitter id must be a string");
        ids.push_back(std::move(id));
    }
    std::string error;
    if (!handle->collector->river_builder->set_section_emitters(
            handle->section, ids, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue section_waterfall(JSContext* context, JSValueConst this_value,
                          int argument_count, JSValueConst* arguments) {
    RiverSectionHandle* handle = active_section_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    RiverWaterfallDefinition waterfall{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_float(context, arguments[0], "lipAt",
                        waterfall.lip_distance_m) ||
        !required_float(context, arguments[0], "landingAt",
                        waterfall.landing_distance_m) ||
        !required_float(context, arguments[0], "expectedDrop",
                        waterfall.expected_drop_m))
        return river_failure(context, handle->collector,
                             "hydrology.section.waterfall: waterfall requires lipAt/landingAt/expectedDrop");
    std::string error;
    if (!handle->collector->river_builder->add_section_waterfall(
            handle->section, waterfall, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue section_pool(JSContext* context, JSValueConst this_value,
                     int argument_count, JSValueConst* arguments) {
    RiverSectionHandle* handle = active_section_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    RiverPoolDefinition pool{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_float(context, arguments[0], "from", pool.start_distance_m) ||
        !required_float(context, arguments[0], "to", pool.end_distance_m) ||
        !required_float(context, arguments[0], "fillLevel", pool.fill_level_m))
        return river_failure(context, handle->collector,
                             "hydrology.section.pool: pool requires from/to/fillLevel");
    std::string error;
    if (!handle->collector->river_builder->set_section_pool(
            handle->section, pool, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue section_spillway(JSContext* context, JSValueConst this_value,
                         int argument_count, JSValueConst* arguments) {
    RiverSectionHandle* handle = active_section_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    RiverSpillwayDefinition spillway{};
    JSValue id_value = argument_count > 0 && JS_IsObject(arguments[0])
        ? JS_GetPropertyStr(context, arguments[0], "id") : JS_UNDEFINED;
    const bool id_ok = string_value(context, id_value, spillway.id);
    JS_FreeValue(context, id_value);
    if (argument_count < 1 || !JS_IsObject(arguments[0]) || !id_ok ||
        !required_float(context, arguments[0], "at", spillway.distance_m) ||
        !required_float(context, arguments[0], "width", spillway.width_m) ||
        !required_float(context, arguments[0], "effectiveDepth",
                        spillway.effective_depth_m) ||
        !required_float(context, arguments[0], "overlap", spillway.overlap_m) ||
        !required_float(context, arguments[0], "damOffset",
                        spillway.dam_offset_m))
        return river_failure(context, handle->collector,
                             "hydrology.section.spillway: spillway requires id/at/width/effectiveDepth/overlap/damOffset");
    std::string error;
    if (!handle->collector->river_builder->set_section_spillway(
            handle->section, spillway, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue section_after(JSContext* context, JSValueConst this_value,
                      int argument_count, JSValueConst* arguments) {
    RiverSectionHandle* handle = active_section_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    std::string upstream;
    if (argument_count < 1 || !string_value(context, arguments[0], upstream))
        return river_failure(context, handle->collector,
                             "hydrology.section.after: after requires a section id");
    std::string error;
    if (!handle->collector->river_builder->add_section_after(
            handle->section, upstream, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue section_from_spillway(JSContext* context, JSValueConst this_value,
                              int argument_count, JSValueConst* arguments) {
    RiverSectionHandle* handle = active_section_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    std::string upstream;
    if (argument_count < 1 || !string_value(context, arguments[0], upstream))
        return river_failure(context, handle->collector,
                             "hydrology.section.fromSpillway: fromSpillway requires a section id");
    std::string error;
    if (!handle->collector->river_builder->add_section_from_spillway(
            handle->section, upstream, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue make_section_object(JSContext* context, LoadCollector* collector,
                            std::size_t section) {
    JSValue object = JS_NewObjectClass(context, river_section_class_id);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new RiverSectionHandle{collector, section});
    JS_SetPropertyStr(context, object, "emitters",
                      JS_NewCFunction(context, section_emitters, "emitters", 1));
    JS_SetPropertyStr(context, object, "waterfall",
                      JS_NewCFunction(context, section_waterfall, "waterfall", 1));
    JS_SetPropertyStr(context, object, "pool",
                      JS_NewCFunction(context, section_pool, "pool", 1));
    JS_SetPropertyStr(context, object, "spillway",
                      JS_NewCFunction(context, section_spillway, "spillway", 1));
    JS_SetPropertyStr(context, object, "after",
                      JS_NewCFunction(context, section_after, "after", 1));
    JS_SetPropertyStr(context, object, "fromSpillway",
                      JS_NewCFunction(context, section_from_spillway,
                                      "fromSpillway", 1));
    return object;
}

JSValue river_section(JSContext* context, JSValueConst this_value,
                      int argument_count, JSValueConst* arguments) {
    RiverHandle* handle = static_cast<RiverHandle*>(
        JS_GetOpaque2(context, this_value, river_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    std::string id;
    float from_m = 0.0f;
    float to_m = 0.0f;
    float dry_margin_m = 0.0f;
    if (argument_count < 2 || !string_value(context, arguments[0], id) ||
        !JS_IsObject(arguments[1]) ||
        !required_float(context, arguments[1], "from", from_m) ||
        !required_float(context, arguments[1], "to", to_m) ||
        !required_float(context, arguments[1], "dryMargin", dry_margin_m))
        return river_failure(context, handle->collector,
                             "hydrology.section: section requires id and from/to/dryMargin");
    std::size_t section = 0u;
    std::string error;
    if (!handle->collector->river_builder->add_section(
            handle->river, id, from_m, to_m, dry_margin_m, section, error))
        return river_failure(context, handle->collector, error);
    return make_section_object(context, handle->collector, section);
}

RiverNetworkHandle* active_network_handle(JSContext* context,
                                          JSValueConst this_value) {
    RiverNetworkHandle* handle = static_cast<RiverNetworkHandle*>(
        JS_GetOpaque2(context, this_value, river_network_class_id));
    if (!handle) return nullptr;
    if (!handle->collector->river_hydrology_active) {
        river_phase_failure(context, handle->collector);
        return nullptr;
    }
    return handle;
}

bool required_string(JSContext* context, JSValueConst object, const char* key,
                     std::string& output) {
    JSValue value = JS_GetPropertyStr(context, object, key);
    const bool ok = string_value(context, value, output);
    JS_FreeValue(context, value);
    return ok;
}

bool required_float3(JSContext* context, JSValueConst object, const char* key,
                     Float3& output) {
    JSValue value = JS_GetPropertyStr(context, object, key);
    const bool ok = float3_value(context, value, output);
    JS_FreeValue(context, value);
    return ok;
}

JSValue network_backend(JSContext* context, JSValueConst this_value,
                        int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    std::string name;
    if (argument_count < 1 || !string_value(context, arguments[0], name) ||
        (name != "physx" && name != "disabled")) {
        return river_failure(context, handle->collector,
                             "hydrology.backend: backend must be 'physx' or 'disabled'");
    }
    std::string error;
    const matter::HydrologyBackend backend = name == "physx"
        ? matter::HydrologyBackend::Physx
        : matter::HydrologyBackend::Disabled;
    if (!handle->collector->river_builder->set_backend(backend, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_pbd(JSContext* context, JSValueConst this_value,
                    int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyPbdSettings settings{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_float(context, arguments[0], "particleSpacing",
                        settings.particle_spacing_m) ||
        !required_float(context, arguments[0], "restDensity",
                        settings.rest_density_kg_m3) ||
        !required_float(context, arguments[0], "fixedStep",
                        settings.fixed_step_seconds) ||
        !required_uint32(context, arguments[0], "iterations",
                         settings.solver_iterations) ||
        !required_uint32(context, arguments[0], "maxNeighbors",
                         settings.max_neighbors)) {
        return river_failure(context, handle->collector,
                             "hydrology.pbd: pbd requires particleSpacing/restDensity/fixedStep/iterations/maxNeighbors");
    }
    std::string error;
    if (!handle->collector->river_builder->set_pbd(settings, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_limits(JSContext* context, JSValueConst this_value,
                       int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyBakeLimits limits{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_uint32(context, arguments[0], "batchSteps", limits.batch_steps) ||
        !required_uint32(context, arguments[0], "maxSteps", limits.max_steps) ||
        !required_uint32(context, arguments[0], "maxParticles", limits.max_particles)) {
        return river_failure(context, handle->collector,
                             "hydrology.limits: limits requires batchSteps/maxSteps/maxParticles");
    }
    std::string error;
    if (!handle->collector->river_builder->set_limits(limits, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_emitter(JSContext* context, JSValueConst this_value,
                        int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyEmitter emitter{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_string(context, arguments[0], "id", emitter.id) ||
        !required_float3(context, arguments[0], "position", emitter.position_m) ||
        !required_float3(context, arguments[0], "direction", emitter.direction) ||
        !required_float3(context, arguments[0], "initialVelocity",
                         emitter.initial_velocity_mps) ||
        !required_float(context, arguments[0], "flow", emitter.flow_m3s) ||
        !required_float(context, arguments[0], "radius", emitter.radius_m) ||
        !required_float(context, arguments[0], "startTime", emitter.start_time_s) ||
        !required_float(context, arguments[0], "stopTime", emitter.stop_time_s)) {
        return river_failure(context, handle->collector,
                             "hydrology.emitter: emitter requires id/position/direction/initialVelocity/flow/radius/startTime/stopTime");
    }
    std::string error;
    if (!handle->collector->river_builder->add_emitter(emitter, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_escape_policy(JSContext* context, JSValueConst this_value,
                              int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyEscapePolicy policy{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_uint32(context, arguments[0], "absoluteCount",
                         policy.absolute_count) ||
        !required_float(context, arguments[0], "ratio", policy.ratio)) {
        return river_failure(
            context, handle->collector,
            "hydrology.escapePolicy: escapePolicy requires absoluteCount/ratio");
    }
    std::string error;
    if (!handle->collector->river_builder->set_escape_policy(policy, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_virtual_dam(JSContext* context, JSValueConst this_value,
                            int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyVirtualDam dam{};
    if (argument_count > 0 && JS_IsObject(arguments[0]) &&
        has_property(context, arguments[0], "distance"))
        return river_failure(context, handle->collector,
                             "hydrology.virtualDam.distance: section spillways own dam distance");
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_float(context, arguments[0], "height", dam.height_m) ||
        !required_float(context, arguments[0], "thickness", dam.thickness_m)) {
        return river_failure(context, handle->collector,
                             "hydrology.virtualDam: virtualDam requires height/thickness");
    }
    std::string error;
    if (!handle->collector->river_builder->set_virtual_dam(dam, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_bake_sequential(JSContext* context, JSValueConst this_value,
                                int, JSValueConst*) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    std::string error;
    if (!handle->collector->river_builder->set_bake_sequential(error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_fill_sensor(JSContext* context, JSValueConst this_value,
                            int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyFillSensor sensor{};
    Float3 resolution{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_float(context, arguments[0], "upstreamOffset",
                        sensor.upstream_offset_m) ||
        !required_float(context, arguments[0], "length", sensor.length_m) ||
        !required_float(context, arguments[0], "height", sensor.height_m) ||
        !required_float3(context, arguments[0], "resolution", resolution) ||
        !std::isfinite(resolution.x) || !std::isfinite(resolution.y) ||
        !std::isfinite(resolution.z) || resolution.x < 0.0f ||
        resolution.y < 0.0f || resolution.z < 0.0f ||
        std::floor(resolution.x) != resolution.x ||
        std::floor(resolution.y) != resolution.y ||
        std::floor(resolution.z) != resolution.z ||
        resolution.x > static_cast<float>(std::numeric_limits<std::uint32_t>::max()) ||
        resolution.y > static_cast<float>(std::numeric_limits<std::uint32_t>::max()) ||
        resolution.z > static_cast<float>(std::numeric_limits<std::uint32_t>::max()) ||
        !required_float(context, arguments[0], "crestWetFraction",
                        sensor.crest_wet_fraction) ||
        !required_uint32(context, arguments[0], "stableWetSteps",
                         sensor.stable_wet_steps) ||
        !required_uint32(context, arguments[0], "minimumParticlesPerCell",
                         sensor.minimum_particles_per_cell)) {
        return river_failure(context, handle->collector,
                             "hydrology.fillSensor: fillSensor requires upstreamOffset/length/height/resolution/crestWetFraction/stableWetSteps/minimumParticlesPerCell");
    }
    sensor.resolution_x = static_cast<std::uint32_t>(resolution.x);
    sensor.resolution_y = static_cast<std::uint32_t>(resolution.y);
    sensor.resolution_z = static_cast<std::uint32_t>(resolution.z);
    std::string error;
    if (!handle->collector->river_builder->set_fill_sensor(sensor, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_quality(JSContext* context, JSValueConst this_value,
                        int argument_count, JSValueConst* arguments) {
    RiverNetworkHandle* handle = active_network_handle(context, this_value);
    if (!handle) return JS_EXCEPTION;
    matter::HydrologyQualitySettings quality{};
    if (argument_count < 1 || !JS_IsObject(arguments[0]) ||
        !required_float(context, arguments[0], "particleRadius",
                        quality.particle_radius_m) ||
        !required_float(context, arguments[0], "visualVoxel",
                        quality.visual_voxel_m) ||
        !required_float(context, arguments[0], "visualBlendWidth",
                        quality.visual_blend_width_m) ||
        !required_float(context, arguments[0], "coarseVoxel",
                        quality.coarse_voxel_m) ||
        !required_float(context, arguments[0], "gameplayCell",
                        quality.gameplay_cell_m) ||
        !required_uint32(context, arguments[0], "maxVisualParticles",
                         quality.max_visual_particles) ||
        !required_uint32(context, arguments[0], "maxGridVertices",
                         quality.max_grid_vertices) ||
        !required_uint32(context, arguments[0], "maxMeshVertices",
                         quality.max_mesh_vertices) ||
        !required_uint32(context, arguments[0], "maxMeshIndices",
                         quality.max_mesh_indices)) {
        return river_failure(context, handle->collector,
                             "hydrology.quality: quality requires all particle/visual/coarse/gameplay settings and caps");
    }
    std::string error;
    if (!handle->collector->river_builder->set_quality(quality, error))
        return river_failure(context, handle->collector, error);
    return JS_DupValue(context, this_value);
}

JSValue network_build(JSContext* context, JSValueConst this_value,
                      int, JSValueConst*) {
    RiverNetworkHandle* handle = static_cast<RiverNetworkHandle*>(
        JS_GetOpaque2(context, this_value, river_network_class_id));
    if (!handle) return JS_EXCEPTION;
    if (!handle->collector->river_hydrology_active)
        return river_phase_failure(context, handle->collector);
    RiverNetworkDefinition definition;
    std::string error;
    if (!handle->collector->river_builder->finish(definition, error))
        return river_failure(context, handle->collector, error);
    handle->collector->finished_river_network = std::move(definition);
    handle->collector->river_network_built = true;
    return JS_UNDEFINED;
}

JSValue river_network(JSContext* context, JSValueConst,
                      int argument_count, JSValueConst* arguments) {
    LoadCollector* collector =
        static_cast<LoadCollector*>(JS_GetContextOpaque(context));
    if (!collector)
        return JS_ThrowInternalError(context, "river network collector unavailable");
    if (!collector->river_hydrology_active)
        return river_phase_failure(context, collector);
    if (collector->river_network_created)
        return river_failure(context, collector,
                             "hydrology.riverNetwork: only one network may be created");
    float cell_size = 0.0f;
    JSValue seed_value = argument_count > 0 && JS_IsObject(arguments[0])
        ? JS_GetPropertyStr(context, arguments[0], "seed") : JS_UNDEFINED;
    double seed_number = 0.0;
    const bool seed_ok = JS_IsNumber(seed_value) &&
                         JS_ToFloat64(context, &seed_number, seed_value) == 0 &&
                         std::isfinite(seed_number) && seed_number >= 0.0 &&
                         std::floor(seed_number) == seed_number &&
                         seed_number <= 9007199254740991.0;
    JS_FreeValue(context, seed_value);
    if (argument_count < 1 || !JS_IsObject(arguments[0])) {
        return river_failure(context, collector,
                             "hydrology.riverNetwork: options object is required");
    }
    if (!required_float(context, arguments[0], "cellSize", cell_size))
        return river_failure(context, collector,
                             "hydrology.cellSize: cellSize must be a finite number");
    if (!seed_ok)
        return river_failure(context, collector,
                             "hydrology.seed: seed must be a nonnegative safe integer");
    collector->river_network_created = true;
    collector->river_builder = std::make_unique<hydrology::RiverNetworkBuilder>(
        cell_size, static_cast<std::uint64_t>(seed_number));
    JSValue object = JS_NewObjectClass(context, river_network_class_id);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new RiverNetworkHandle{collector});
    JS_SetPropertyStr(context, object, "river",
                      JS_NewCFunction(context, network_river, "river", 1));
    JS_SetPropertyStr(context, object, "backend",
                      JS_NewCFunction(context, network_backend, "backend", 1));
    JS_SetPropertyStr(context, object, "pbd",
                      JS_NewCFunction(context, network_pbd, "pbd", 1));
    JS_SetPropertyStr(context, object, "limits",
                      JS_NewCFunction(context, network_limits, "limits", 1));
    JS_SetPropertyStr(context, object, "escapePolicy",
                      JS_NewCFunction(context, network_escape_policy,
                                      "escapePolicy", 1));
    JS_SetPropertyStr(context, object, "emitter",
                      JS_NewCFunction(context, network_emitter, "emitter", 1));
    JS_SetPropertyStr(context, object, "virtualDam",
                      JS_NewCFunction(context, network_virtual_dam, "virtualDam", 1));
    JS_SetPropertyStr(context, object, "fillSensor",
                      JS_NewCFunction(context, network_fill_sensor, "fillSensor", 1));
    JS_SetPropertyStr(context, object, "quality",
                      JS_NewCFunction(context, network_quality, "quality", 1));
    JS_SetPropertyStr(context, object, "bakeSequential",
                      JS_NewCFunction(context, network_bake_sequential,
                                      "bakeSequential", 0));
    JS_SetPropertyStr(context, object, "build",
                      JS_NewCFunction(context, network_build, "build", 0));
    return object;
}

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

bool valid_fluid_collider_transform(const Mat4f& transform) {
    for (const float value : transform.m)
        if (!std::isfinite(value)) return false;
    if (transform.m[12] != 0.0f || transform.m[13] != 0.0f ||
        transform.m[14] != 0.0f || transform.m[15] != 1.0f)
        return false;
    const Float3 columns[] = {
        {transform.m[0], transform.m[4], transform.m[8]},
        {transform.m[1], transform.m[5], transform.m[9]},
        {transform.m[2], transform.m[6], transform.m[10]},
    };
    const auto dot = [](Float3 a, Float3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    float lengths[3]{};
    for (std::size_t index = 0; index < 3u; ++index) {
        lengths[index] = std::sqrt(dot(columns[index], columns[index]));
        if (!(lengths[index] > 1.0e-6f)) return false;
    }
    for (std::size_t a = 0; a < 3u; ++a) {
        for (std::size_t b = a + 1u; b < 3u; ++b) {
            const float tolerance = 1.0e-4f * lengths[a] * lengths[b];
            if (std::fabs(dot(columns[a], columns[b])) > tolerance)
                return false;
        }
    }
    return true;
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
    std::set<std::string> fluid_collider_ids;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::string path = "roots[" + std::to_string(index) + "]";
        JSValue entry = JS_GetPropertyUint32(context, roots, index);
        if (!JS_IsObject(entry)) {
            JS_FreeValue(context, entry);
            JS_FreeValue(context, roots);
            return fail(desc, error, path, "world root must be an object");
        }
        WorldRoot root;
        JSValue id = JS_GetPropertyStr(context, entry, "id");
        if (!JS_IsUndefined(id) && !string_value(context, id, root.id)) {
            JS_FreeValue(context, id);
            JS_FreeValue(context, entry);
            JS_FreeValue(context, roots);
            return fail(desc, error, path + ".id",
                        "world root id must be a string");
        }
        JS_FreeValue(context, id);
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

        JSValue fluid_collider =
            JS_GetPropertyStr(context, entry, "fluidCollider");
        if (!JS_IsUndefined(fluid_collider)) {
            if (root.id.empty()) {
                JS_FreeValue(context, fluid_collider);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".id",
                            "fluid collider roots require a non-empty stable id");
            }
            if (!JS_IsObject(fluid_collider)) {
                JS_FreeValue(context, fluid_collider);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".fluidCollider",
                            "fluidCollider must be an object");
            }
            JSValue shape =
                JS_GetPropertyStr(context, fluid_collider, "shape");
            std::string shape_name;
            if (!string_value(context, shape, shape_name)) {
                JS_FreeValue(context, shape);
                JS_FreeValue(context, fluid_collider);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".fluidCollider.shape",
                            "fluid collider shape must be a string");
            }
            JS_FreeValue(context, shape);
            if (shape_name == "sphere") {
                JSValue radius =
                    JS_GetPropertyStr(context, fluid_collider, "radius");
                const bool valid = number_value(
                    context, radius, root.fluid_collider.radius_m);
                JS_FreeValue(context, radius);
                if (!valid || !std::isfinite(root.fluid_collider.radius_m) ||
                    root.fluid_collider.radius_m <= 0.0f) {
                    JS_FreeValue(context, fluid_collider);
                    JS_FreeValue(context, entry);
                    JS_FreeValue(context, roots);
                    return fail(desc, error,
                                path + ".fluidCollider.radius",
                                "sphere radius must be finite and positive");
                }
                root.fluid_collider.shape =
                    WorldFluidColliderShape::Sphere;
            } else if (shape_name == "box") {
                JSValue extents = JS_GetPropertyStr(
                    context, fluid_collider, "halfExtents");
                const bool valid = float3_value(
                    context, extents,
                    root.fluid_collider.half_extents_m);
                JS_FreeValue(context, extents);
                const auto half = root.fluid_collider.half_extents_m;
                if (!valid || !std::isfinite(half.x) ||
                    !std::isfinite(half.y) || !std::isfinite(half.z) ||
                    half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) {
                    JS_FreeValue(context, fluid_collider);
                    JS_FreeValue(context, entry);
                    JS_FreeValue(context, roots);
                    return fail(desc, error,
                                path + ".fluidCollider.halfExtents",
                                "box halfExtents must be three finite positive numbers");
                }
                root.fluid_collider.shape = WorldFluidColliderShape::Box;
            } else {
                JS_FreeValue(context, fluid_collider);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".fluidCollider.shape",
                            "fluid collider shape must be sphere or box");
            }
            JSValue center =
                JS_GetPropertyStr(context, fluid_collider, "center");
            if (!JS_IsUndefined(center)) {
                const bool valid = float3_value(
                    context, center, root.fluid_collider.center_m);
                const auto local_center = root.fluid_collider.center_m;
                if (!valid || !std::isfinite(local_center.x) ||
                    !std::isfinite(local_center.y) ||
                    !std::isfinite(local_center.z)) {
                    JS_FreeValue(context, center);
                    JS_FreeValue(context, fluid_collider);
                    JS_FreeValue(context, entry);
                    JS_FreeValue(context, roots);
                    return fail(desc, error,
                                path + ".fluidCollider.center",
                                "fluid collider center must be three finite numbers");
                }
            }
            JS_FreeValue(context, center);
            if (!valid_fluid_collider_transform(root.transform)) {
                JS_FreeValue(context, fluid_collider);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".transform",
                            "fluid collider transform must be finite, affine, nonsingular, and shear-free");
            }
            if (!fluid_collider_ids.insert(root.id).second) {
                JS_FreeValue(context, fluid_collider);
                JS_FreeValue(context, entry);
                JS_FreeValue(context, roots);
                return fail(desc, error, path + ".id",
                            "fluid collider root ids must be unique");
            }
        }
        JS_FreeValue(context, fluid_collider);

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
            {"weatherScale", &layer.weather_scale},
            {"weatherInfluence", &layer.weather_influence},
            {"detailScale", &layer.detail_scale},
            {"detailErosion", &layer.detail_erosion},
            {"shapeBias", &layer.shape_bias},
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

    // Volumetric sectors (volumetric-sectors M3). Same placement and the same
    // reason: it is nesting with a third axis, so it lives before the rings
    // early-out for exactly the case nesting does.
    //
    // NOT validated against nestedSectors here. The loader's job is to report
    // what the world authored; the streamer owns whether the combination is
    // coherent and forces this off when there is no ladder to descend, which is
    // where the same decision already lives for terrain_lod_enabled.
    {
        JSValue volumetric =
            JS_GetPropertyStr(context, streaming, "volumetricSectors");
        if (!JS_IsUndefined(volumetric))
            definition.settings.volumetric_sectors =
                JS_ToBool(context, volumetric) != 0;
        JS_FreeValue(context, volumetric);
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

    MATTER_LOGW("fog",
                 "%s: volumetrics.fogDensityMul/fogFalloffMul/"
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

bool extract_atmosphere(JSContext* context,
                        JSValueConst world_class,
                        const WorldLoadDesc& desc,
                        WorldDefinition& definition,
                        WorldLoadError& error) {
    JSValue atmosphere = JS_GetPropertyStr(context, world_class, "atmosphere");
    if (JS_IsUndefined(atmosphere)) {
        JS_FreeValue(context, atmosphere);
        return true;
    }
    if (!JS_IsObject(atmosphere)) {
        JS_FreeValue(context, atmosphere);
        return fail(desc, error, "atmosphere", "World.atmosphere must be an object");
    }
    AtmosphereSettings& out = definition.settings.atmosphere;
    const struct { const char* name; float* target; } fields[] = {
        {"seaLevelY", &out.sea_level_y}, {"rayleighScale", &out.rayleigh_scale},
        {"mieScale", &out.mie_scale}, {"mieAnisotropy", &out.mie_anisotropy},
        {"ozoneScale", &out.ozone_scale}, {"groundAlbedo", &out.ground_albedo},
    };
    for (const auto& field : fields) {
        JSValue value = JS_GetPropertyStr(context, atmosphere, field.name);
        if (!JS_IsUndefined(value) &&
            (!number_value(context, value, *field.target) || !std::isfinite(*field.target))) {
            JS_FreeValue(context, value);
            JS_FreeValue(context, atmosphere);
            return fail(desc, error, std::string("atmosphere.") + field.name,
                        "must be a finite number");
        }
        JS_FreeValue(context, value);
    }
    JS_FreeValue(context, atmosphere);
    return true;
}

bool extract_cloud_shadows(JSContext* context,
                           JSValueConst world_class,
                           const WorldLoadDesc& desc,
                           WorldDefinition& definition,
                           WorldLoadError& error) {
    JSValue shadows = JS_GetPropertyStr(context, world_class, "cloudShadows");
    if (JS_IsUndefined(shadows)) {
        JS_FreeValue(context, shadows);
        return true;
    }
    if (!JS_IsObject(shadows)) {
        JS_FreeValue(context, shadows);
        return fail(desc, error, "cloudShadows", "World.cloudShadows must be an object");
    }
    CloudShadowSettings& out = definition.settings.cloud_shadows;
    JSValue enabled = JS_GetPropertyStr(context, shadows, "enabled");
    if (!JS_IsUndefined(enabled)) out.enabled = JS_ToBool(context, enabled);
    JS_FreeValue(context, enabled);

    const struct { const char* name; float* target; } numbers[] = {
        {"nearCoverageM", &out.near_coverage_m}, {"farCoverageM", &out.far_coverage_m},
        {"filterScale", &out.filter_scale}, {"updateFraction", &out.update_fraction},
    };
    for (const auto& number : numbers) {
        JSValue value = JS_GetPropertyStr(context, shadows, number.name);
        if (!JS_IsUndefined(value) &&
            (!number_value(context, value, *number.target) || !std::isfinite(*number.target))) {
            JS_FreeValue(context, value);
            JS_FreeValue(context, shadows);
            return fail(desc, error, std::string("cloudShadows.") + number.name,
                        "must be a finite number");
        }
        JS_FreeValue(context, value);
    }
    if (!(out.near_coverage_m > 0.0f)) {
        JS_FreeValue(context, shadows);
        return fail(desc, error, "cloudShadows.nearCoverageM", "must be positive");
    }
    if (!(out.far_coverage_m > 0.0f)) {
        JS_FreeValue(context, shadows);
        return fail(desc, error, "cloudShadows.farCoverageM", "must be positive");
    }
    if (!(out.filter_scale > 0.0f)) {
        JS_FreeValue(context, shadows);
        return fail(desc, error, "cloudShadows.filterScale", "must be positive");
    }
    if (out.update_fraction < 0.0f || out.update_fraction > 1.0f) {
        JS_FreeValue(context, shadows);
        return fail(desc, error, "cloudShadows.updateFraction", "must be in [0,1]");
    }

    const struct { const char* name; int32_t* target; int values[3]; } discrete[] = {
        {"nearResolution", &out.near_resolution, {128, 256, 512}},
        {"nearDepthSlices", &out.near_depth_slices, {16, 32, 48}},
        {"farResolution", &out.far_resolution, {64, 128, 256}},
        {"farDepthSlices", &out.far_depth_slices, {16, 24, 32}},
    };
    for (const auto& field : discrete) {
        JSValue value = JS_GetPropertyStr(context, shadows, field.name);
        if (!JS_IsUndefined(value)) {
            float parsed = 0.0f;
            int32_t index = -1;
            if (number_value(context, value, parsed) && std::isfinite(parsed)) {
                for (int32_t i = 0; i < 3; ++i)
                    if (parsed == static_cast<float>(field.values[i])) index = i;
            }
            JS_FreeValue(context, value);
            if (index < 0) {
                JS_FreeValue(context, shadows);
                return fail(desc, error, std::string("cloudShadows.") + field.name,
                            "has an unrecognized discrete value");
            }
            *field.target = index;
            continue;
        }
        JS_FreeValue(context, value);
    }
    JS_FreeValue(context, shadows);
    return true;
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
        {"localSunMarchDistanceM", &out.local_sun_march_distance_m},
        {"multipleScatteringStrength", &out.multiple_scattering_strength},
        {"powderStrength", &out.powder_strength},
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
    const struct { const char* name; int32_t* target; int32_t minimum; int32_t maximum; } integers[] = {
        {"localSunMarchSteps", &out.local_sun_march_steps, 0, 24},
        {"multipleScatteringOrders", &out.multiple_scattering_orders, 1, 4},
    };
    for (const auto& field : integers) {
        JSValue value = JS_GetPropertyStr(context, vol, field.name);
        if (!JS_IsUndefined(value)) {
            float parsed = 0.0f;
            if (!number_value(context, value, parsed) || !std::isfinite(parsed) ||
                std::floor(parsed) != parsed || parsed < field.minimum || parsed > field.maximum) {
                JS_FreeValue(context, value);
                JS_FreeValue(context, vol);
                return fail(desc, error, std::string("volumetrics.") + field.name,
                            "must be a supported integer");
            }
            *field.target = static_cast<int32_t>(parsed);
        }
        JS_FreeValue(context, value);
    }
    JSValue froxel_xy = JS_GetPropertyStr(context, vol, "froxelXyScale");
    if (!JS_IsUndefined(froxel_xy)) {
        std::string label;
        if (!string_value(context, froxel_xy, label)) {
            JS_FreeValue(context, froxel_xy); JS_FreeValue(context, vol);
            return fail(desc, error, "volumetrics.froxelXyScale", "must be a recognized scale label");
        }
        const struct { const char* label; FroxelXyScale value; } labels[] = {
            {"0.5x", FroxelXyScale::X0_5}, {"0.75x", FroxelXyScale::X0_75},
            {"1x", FroxelXyScale::X1_0}, {"1.5x", FroxelXyScale::X1_5},
            {"2x", FroxelXyScale::X2_0},
        };
        bool found = false;
        for (const auto& entry : labels) if (label == entry.label) {
            out.froxel_xy_scale = entry.value; found = true; break;
        }
        JS_FreeValue(context, froxel_xy);
        if (!found) { JS_FreeValue(context, vol); return fail(desc, error, "volumetrics.froxelXyScale", "must be a recognized scale label"); }
    } else JS_FreeValue(context, froxel_xy);
    JSValue froxel_depth = JS_GetPropertyStr(context, vol, "froxelDepthSlices");
    if (!JS_IsUndefined(froxel_depth)) {
        float parsed = 0.0f;
        int index = -1;
        if (number_value(context, froxel_depth, parsed) && std::isfinite(parsed)) {
            const int values[] = {64, 96, 128, 192, 256};
            for (int i = 0; i < 5; ++i) if (parsed == static_cast<float>(values[i])) index = i;
        }
        JS_FreeValue(context, froxel_depth);
        if (index < 0) { JS_FreeValue(context, vol); return fail(desc, error, "volumetrics.froxelDepthSlices", "must be a supported slice count"); }
        out.froxel_depth_slices = static_cast<FroxelDepthSlices>(index);
    } else JS_FreeValue(context, froxel_depth);
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

// Hydrology is deliberately a narrow, plain-data static. It is not a general
// configuration object: later bake stages key the exact values below, so a
// getter, function, inherited value, or unknown key would make definition-time
// evaluation non-hermetic or silently change the frozen contract.
bool hydrology_plain_data(JSContext* context, JSValueConst value,
                          unsigned depth = 0) {
    if (depth > 8 || JS_IsFunction(context, value) || JS_IsProxy(value))
        return false;
    if (!JS_IsObject(value)) return true;
    JSPropertyEnum* names = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &names, &count, value,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) != 0)
        return false;
    bool ok = true;
    for (std::uint32_t index = 0; index < count && ok; ++index) {
        JSPropertyDescriptor property{};
        if (JS_GetOwnProperty(context, &property, value, names[index].atom) != 1 ||
            (property.flags & JS_PROP_GETSET)) {
            ok = false;
        } else {
            ok = hydrology_plain_data(context, property.value, depth + 1);
        }
        JS_FreeValue(context, property.value);
        JS_FreeValue(context, property.getter);
        JS_FreeValue(context, property.setter);
    }
    JS_FreePropertyEnum(context, names, count);
    return ok;
}

bool hydrology_own_data_value(JSContext* context, JSValueConst object,
                              const char* key, JSValue& value, bool& present) {
    JSAtom atom = JS_NewAtom(context, key);
    JSPropertyDescriptor property{};
    const int found = JS_GetOwnProperty(context, &property, object, atom);
    JS_FreeAtom(context, atom);
    if (found != 1) {
        present = false;
        value = JS_UNDEFINED;
        return found == 0;
    }
    present = true;
    if (property.flags & JS_PROP_GETSET) {
        JS_FreeValue(context, property.value);
        JS_FreeValue(context, property.getter);
        JS_FreeValue(context, property.setter);
        value = JS_UNDEFINED;
        return false;
    }
    value = property.value;
    JS_FreeValue(context, property.getter);
    JS_FreeValue(context, property.setter);
    return true;
}

bool hydrology_number(JSContext* context, JSValueConst value, float& output) {
    if (!JS_IsNumber(value)) return false;
    double number = 0.0;
    if (JS_ToFloat64(context, &number, value) != 0 || !std::isfinite(number) ||
        number < -std::numeric_limits<float>::max() ||
        number > std::numeric_limits<float>::max())
        return false;
    output = static_cast<float>(number);
    return true;
}

bool hydrology_uint32(JSContext* context, JSValueConst value, std::uint32_t& output) {
    if (!JS_IsNumber(value)) return false;
    double number = 0.0;
    if (JS_ToFloat64(context, &number, value) != 0 || !std::isfinite(number) ||
        number < 0.0 || std::floor(number) != number ||
        number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return false;
    output = static_cast<std::uint32_t>(number);
    return true;
}

bool hydrology_float_array(JSContext* context, JSValueConst value,
                           std::uint32_t expected, float* output) {
    std::uint32_t length = 0;
    if (!array_length(context, value, length) || length != expected) return false;
    for (std::uint32_t index = 0; index < expected; ++index) {
        JSValue item = JS_GetPropertyUint32(context, value, index);
        const bool ok = hydrology_number(context, item, output[index]);
        JS_FreeValue(context, item);
        if (!ok) return false;
    }
    return true;
}

bool hydrology_dimensions(JSContext* context, JSValueConst value,
                          matter::HydrologyDomainSettings& domain) {
    std::uint32_t* out[] = {&domain.nx, &domain.ny, &domain.nz};
    std::uint32_t length = 0;
    if (!array_length(context, value, length) || length != 3) return false;
    for (unsigned index = 0; index < 3; ++index) {
        JSValue item = JS_GetPropertyUint32(context, value, index);
        const bool ok = hydrology_uint32(context, item, *out[index]);
        JS_FreeValue(context, item);
        if (!ok) return false;
    }
    return true;
}

bool reject_unknown_hydrology_keys(JSContext* context, JSValueConst value,
                                   const char* const* keys, std::size_t key_count,
                                   std::string& unknown) {
    JSPropertyEnum* properties = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &count, value,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) != 0)
        return false;
    bool ok = true;
    for (std::uint32_t index = 0; index < count && ok; ++index) {
        JSValue atom_value = JS_AtomToValue(context, properties[index].atom);
        const bool is_symbol = JS_IsSymbol(atom_value);
        JS_FreeValue(context, atom_value);
        if (is_symbol) {
            unknown = "<symbol>";
            ok = false;
            break;
        }
        const char* text = JS_AtomToCString(context, properties[index].atom);
        if (!text) {
            ok = false;
            break;
        }
        bool known = false;
        for (std::size_t key = 0; key < key_count; ++key)
            if (std::strcmp(keys[key], text) == 0) { known = true; break; }
        if (!known) {
            unknown = text;
            ok = false;
        }
        JS_FreeCString(context, text);
    }
    JS_FreePropertyEnum(context, properties, count);
    return ok;
}

bool extract_hydrology(JSContext* context,
                       JSValueConst world_class,
                       const WorldLoadDesc& desc,
                       WorldDefinition& definition,
                       WorldLoadError& error) {
    JSValue value = JS_UNDEFINED;
    bool present = false;
    if (!hydrology_own_data_value(context, world_class, "hydrology", value, present))
        return fail(desc, error, "hydrology", "World.hydrology must be a plain data value");
    if (!present) return true;
    if (!JS_IsObject(value) || JS_IsArray(value) || !hydrology_plain_data(context, value)) {
        JS_FreeValue(context, value);
        return fail(desc, error, "hydrology",
                    "World.hydrology must be a plain data object without accessors or functions");
    }

    static constexpr const char* kHydrologyKeys[] = {
        "enabled", "origin", "dimensions", "cellSize", "dt", "gravity",
        "downstream", "residualGrade", "inletFlow", "inletHead", "outletHead",
        "batchSteps", "maxSteps",
    };
    std::string unknown;
    if (!reject_unknown_hydrology_keys(context, value, kHydrologyKeys,
                                       sizeof(kHydrologyKeys) / sizeof(kHydrologyKeys[0]),
                                       unknown)) {
        JS_FreeValue(context, value);
        return fail(desc, error, "hydrology" + (unknown.empty() ? std::string{} : "." + unknown),
                    unknown.empty() ? "World.hydrology keys could not be read"
                                    : "World.hydrology contains an unknown key");
    }

    matter::HydrologyWorldSettings settings{};
    bool ok = true;
    const auto required = [&](const char* key, JSValue& output) {
        bool key_present = false;
        const bool found = hydrology_own_data_value(context, value, key, output, key_present);
        return found && key_present;
    };
    JSValue field = JS_UNDEFINED;
    if (!required("enabled", field) || !JS_IsBool(field)) ok = false;
    if (ok) settings.enabled = JS_ToBool(context, field) != 0;
    JS_FreeValue(context, field);

    float origin[3]{};
    field = JS_UNDEFINED;
    if (ok && (!required("origin", field) || !hydrology_float_array(context, field, 3, origin))) ok = false;
    JS_FreeValue(context, field);
    settings.domain.origin_m = {origin[0], origin[1], origin[2]};

    field = JS_UNDEFINED;
    if (ok && (!required("dimensions", field) || !hydrology_dimensions(context, field, settings.domain))) ok = false;
    JS_FreeValue(context, field);

    const struct { const char* key; float* target; } numbers[] = {
        {"cellSize", &settings.domain.cell_size_m}, {"dt", &settings.dt_s},
        {"gravity", &settings.gravity_mps2}, {"inletFlow", &settings.inlet_flow_m3s},
        {"inletHead", &settings.inlet_head_m}, {"outletHead", &settings.outlet_head_m},
    };
    for (const auto& item : numbers) {
        field = JS_UNDEFINED;
        if (ok && (!required(item.key, field) || !hydrology_number(context, field, *item.target))) ok = false;
        JS_FreeValue(context, field);
    }

    float downstream[2]{};
    field = JS_UNDEFINED;
    if (ok && (!required("downstream", field) || !hydrology_float_array(context, field, 2, downstream))) ok = false;
    JS_FreeValue(context, field);
    settings.downstream_xz = {downstream[0], downstream[1]};

    float residual_grade[2]{};
    field = JS_UNDEFINED;
    if (ok && (!required("residualGrade", field) || !hydrology_float_array(context, field, 2, residual_grade))) ok = false;
    JS_FreeValue(context, field);
    settings.residual_head_gradient_xz = {residual_grade[0], residual_grade[1]};

    const struct { const char* key; std::uint32_t* target; } steps[] = {
        {"batchSteps", &settings.batch_steps}, {"maxSteps", &settings.max_steps},
    };
    for (const auto& item : steps) {
        field = JS_UNDEFINED;
        if (ok && (!required(item.key, field) ||
                   !hydrology_uint32(context, field, *item.target))) {
            ok = false;
        }
        JS_FreeValue(context, field);
    }
    JS_FreeValue(context, value);
    if (!ok)
        return fail(desc, error, "hydrology",
                    "World.hydrology must contain complete finite typed values");

    hydrology::HydrologyBakeDescription canonical{};
    std::string validation_error;
    if (!hydrology::validate_and_key(settings, 0, canonical, validation_error))
        return fail(desc, error, "hydrology", validation_error);
    definition.hydrology = settings;
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
    if (!install_river_classes(runtime)) {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        return fail(desc, error, "runtime", "unable to install river builder classes");
    }
    auto cleanup = [&]() {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
    };

    // ScriptProfile no-ops (dsl_bindings.h). Nothing here to time -- this
    // context has no DSL bindings at all -- but a world definition imports
    // shared-lib modules that carry prof() calls for their PART-bake path, and
    // those calls run at MODULE SCOPE. Without these, importing such a module
    // throws ReferenceError during world load, which surfaces as "the world
    // failed to load" with no hint that instrumentation caused it.
    //
    // This is the THIRD prelude that needs them: part_base.js.h and
    // world_base.js.h have their own. Anything a shared-lib module may
    // reference at module scope has to exist in all three.
    static constexpr const char* base_source = R"JS(
globalThis.profSlot  = () => -1;
globalThis.profBegin = () => {};
globalThis.profEnd   = () => {};
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
    JS_SetPropertyStr(context, global, "riverNetwork",
                      JS_NewCFunction(context, river_network, "riverNetwork", 1));
    JS_SetPropertyStr(context, global, "terrainCollision",
                      JS_NewCFunction(context, terrain_collision_builder, "terrainCollision", 1));
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
        const std::string path = load_collector.terrain_collision_error_path.empty()
            ? "source" : load_collector.terrain_collision_error_path;
        return fail(desc, error, path, message);
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
              extract_hydrology(context, world_class, desc, definition, error) &&
              extract_lights(context, world_class, desc, definition, error) &&
              extract_atmosphere(context, world_class, desc, definition, error) &&
              extract_fog(context, world_class, desc, definition, error) &&
              extract_camera(context, world_class, desc, definition, error) &&
              extract_streaming(context, world_class, desc, definition, error) &&
              extract_volumetrics(context, world_class, desc, definition,
                                  error) &&
              extract_cloud_shadows(context, world_class, desc, definition,
                                    error) &&
              extract_props(context, world_class, desc, definition, error) &&
              append_static_entities(context, world_class, desc, error);
    // The legacy bounded layer becomes clouds[0] here, not inside
    // extract_fog: it has to run after extract_volumetrics' multiplier fold
    // has finished rewriting minHeight/maxHeight/density, and extract_fog
    // runs before that. Unconditional rather than folded into the chain above
    // because a world can author `fog.minHeight` with no `volumetrics` block
    // at all, and that world still needs its deck.
    if (ok) {
        definition.settings.atmosphere =
            sanitize_atmosphere(definition.settings.atmosphere);
        apply_legacy_height_layer(definition.settings.fog);
    }
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

    JSValue hydrology_method = JS_GetPropertyStr(context, instance, "hydrology");
    if (JS_IsException(hydrology_method)) {
        const std::string message = exception_message(context);
        definition = WorldDefinition{};
        JS_FreeValue(context, hydrology_method);
        JS_FreeValue(context, instance);
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return fail(desc, error, "hydrology", message);
    }
    if (!JS_IsUndefined(hydrology_method)) {
        if (!JS_IsFunction(context, hydrology_method)) {
            definition = WorldDefinition{};
            JS_FreeValue(context, hydrology_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "hydrology",
                        "World.hydrology must be a function when declared on an instance");
        }
        if (definition.hydrology.has_value()) {
            definition = WorldDefinition{};
            JS_FreeValue(context, hydrology_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "hydrology",
                        "static World.hydrology and instance hydrology() are mutually exclusive");
        }
        load_collector.river_error_path.clear();
        load_collector.terrain_collision_phase = "hydrology()";
        load_collector.river_hydrology_active = true;
        JSValue result = JS_Call(context, hydrology_method, instance, 0, nullptr);
        load_collector.river_hydrology_active = false;
        if (JS_IsException(result)) {
            const std::string message = exception_message(context);
            const std::string path = load_collector.river_error_path.empty()
                ? "hydrology" : load_collector.river_error_path;
            definition = WorldDefinition{};
            JS_FreeValue(context, result);
            JS_FreeValue(context, hydrology_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, path, message);
        }
        JS_FreeValue(context, result);
        if (!load_collector.river_network_created ||
            !load_collector.river_network_built ||
            !load_collector.finished_river_network.has_value()) {
            definition = WorldDefinition{};
            JS_FreeValue(context, hydrology_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "hydrology.build",
                        "hydrology() must call exactly one network.build()");
        }
        definition.river_network =
            std::move(load_collector.finished_river_network);
    } else if (load_collector.river_network_created) {
        definition = WorldDefinition{};
        JS_FreeValue(context, hydrology_method);
        JS_FreeValue(context, instance);
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return fail(desc, error, "hydrology",
                    "riverNetwork() may only be used from hydrology()");
    }
    JS_FreeValue(context, hydrology_method);

    JSValue collision_method = JS_GetPropertyStr(context, instance, "collision");
    if (JS_IsException(collision_method)) {
        const std::string message = exception_message(context);
        definition = WorldDefinition{};
        JS_FreeValue(context, collision_method);
        JS_FreeValue(context, instance);
        JS_FreeValue(context, canonicalizer);
        JS_FreeValue(context, world_class);
        cleanup();
        return fail(desc, error, "collision", message);
    }
    if (!JS_IsUndefined(collision_method)) {
        if (!JS_IsFunction(context, collision_method)) {
            definition = WorldDefinition{};
            JS_FreeValue(context, collision_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "collision",
                        "World.collision must be a function when declared on an instance");
        }
        load_collector.terrain_collision_error_path.clear();
        load_collector.terrain_collision_phase = "collision()";
        load_collector.terrain_collision_sector_size_m = definition.settings.sector_size;
        load_collector.terrain_collision_active = true;
        JSValue result = JS_Call(context, collision_method, instance, 0, nullptr);
        load_collector.terrain_collision_active = false;
        if (JS_IsException(result)) {
            const std::string message = exception_message(context);
            const std::string path = load_collector.terrain_collision_error_path.empty()
                ? "collision" : load_collector.terrain_collision_error_path;
            definition = WorldDefinition{};
            JS_FreeValue(context, result);
            JS_FreeValue(context, collision_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, path, message);
        }
        JS_FreeValue(context, result);  // collision() return values are intentionally ignored.
        if (!load_collector.terrain_collision_created ||
            !load_collector.terrain_collision_built) {
            definition = WorldDefinition{};
            JS_FreeValue(context, collision_method);
            JS_FreeValue(context, instance);
            JS_FreeValue(context, canonicalizer);
            JS_FreeValue(context, world_class);
            cleanup();
            return fail(desc, error, "collision.build",
                        "collision() must call exactly one terrainCollision(...).build()");
        }
        definition.terrain_collision = load_collector.terrain_collision_settings;
    }
    JS_FreeValue(context, collision_method);

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
        load_collector.terrain_collision_phase = "buildEntities()";
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
