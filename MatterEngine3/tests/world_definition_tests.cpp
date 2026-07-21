// Phase 4 Task 1: engine-owned World JavaScript statics contract.
#include "check.h"
#include "../src/provider/local_provider.h"
#include "../src/script/world_definition_loader.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Fixture {
    fs::path root;

    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("matter-world-definition-" + std::to_string(stamp));
        fs::create_directories(root / "objects");
        fs::create_directories(root / "project-shared");
        fs::create_directories(root / "engine-shared");
    }

    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path write(const fs::path& relative, const std::string& contents) {
        const fs::path path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << contents;
        return path;
    }

    matter::WorldLoadDesc desc(const fs::path& world_path) const {
        matter::WorldLoadDesc result;
        result.world_path = world_path.string();
        result.objects_dir = (root / "objects").string();
        result.project_shared_lib_dir = (root / "project-shared").string();
        result.engine_shared_lib_dir = (root / "engine-shared").string();
        result.world_seed = 77;
        result.canonical_params_json = "{\"difficulty\":3}";
        return result;
    }
};

void test_project_layout_derives_runtime_paths() {
    Fixture fixture;
    fs::create_directories(fixture.root / "worlds");

    auto cfg = viewer::LocalProviderConfig::for_project(
        fixture.root.string(), "Demo", (fixture.root / "engine-shared").string());

    CHECK(cfg.project_dir == fixture.root.string(),
          "project root is retained for runtime reloads");
    CHECK(cfg.objects_dir == (fixture.root / "objects").string(),
          "object modules come from <project>/objects");
    CHECK(cfg.world_path == (fixture.root / "worlds" / "Demo.js").string(),
          "world source is <project>/worlds/<name>.js");
    CHECK(cfg.cache_root == (fixture.root / ".cache" / "Demo").string(),
          "all generated output is rooted under <project>/.cache/<name>");
    CHECK(cfg.project_shared_lib_dir.empty(),
          "a missing project shared-lib is an empty optional tier");
    CHECK(cfg.engine_shared_lib_dir == (fixture.root / "engine-shared").string(),
          "engine shared-lib remains the fallback tier");

    fs::create_directories(fixture.root / "shared-lib");
    cfg = viewer::LocalProviderConfig::for_project(
        fixture.root.string(), "Demo", (fixture.root / "engine-shared").string());
    CHECK(cfg.project_shared_lib_dir == (fixture.root / "shared-lib").string(),
          "an existing project shared-lib is the preferred tier");
    CHECK(cfg.shared_lib_roots() == std::vector<std::string>({
              (fixture.root / "shared-lib").string(),
              (fixture.root / "engine-shared").string()}),
          "project shared roots preserve project-first engine-fallback order");
}

void test_rejects_non_world_base_with_location_and_property() {
    Fixture fixture;
    const fs::path path = fixture.write(
        "Wrong.js", "class Wrong extends Part { static roots = []; }\n");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;

    CHECK(!matter::load_world_definition(fixture.desc(path), definition, error),
          "non-World base class is rejected");
    CHECK(error.source_location.find("Wrong.js") != std::string::npos,
          "base-class error names its source");
    CHECK(error.property_path == "class",
          "base-class error identifies the class property");
}

void test_extracts_statics_without_calling_field_and_uses_project_override() {
    Fixture fixture;
    fixture.write("project-shared/choice.js",
                  "export const chosen = 'ProjectRoot';\n");
    fixture.write("engine-shared/choice.js",
                  "export const chosen = 'EngineRoot';\n");
    const fs::path path = fixture.write("World.js", R"JS(
import { chosen } from 'shared-lib/choice';
class FixtureWorld extends World {
  static roots = [
    { module: chosen, params: { z: 2, a: 1 },
      transform: [1, 0, 0, 4, 0, 1, 0, 5, 0, 0, 1, 6, 0, 0, 0, 1],
      expand: true },
    { module: 'TileRoot', tileset: true },
  ];
  static lights = [
    { position: [1, 2, 3], color: [0.5, 0.6, 0.7], intensity: 2.5, range: 42 },
  ];
  static settings = { sectorSize: 32, yMin: -12, yMax: 88 };
  static entities = [{
    id: 'static-one', name: 'Static One',
    components: { Zed: { enabled: true }, Alpha: 4 },
  }];
  constructor() { throw new Error('constructor must not run'); }
  field() { throw new Error('field must not run'); }
  buildEntities() {
    if (typeof Date !== 'undefined' || typeof fetch !== 'undefined' ||
        typeof require !== 'undefined' || typeof process !== 'undefined' ||
        typeof Math.random !== 'undefined') {
      throw new Error('ambient authority exposed');
    }
    this.entity({
      id: 'built-one', name: 'Built One', parent: 'static-one',
      components: { SeedProbe: { seed: this.worldSeed, difficulty: this.params.difficulty } },
    });
  }
}
)JS");

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());
    CHECK(definition.roots.size() == 2, "two roots extracted");
    if (definition.roots.size() == 2) {
        CHECK(definition.roots[0].module == "ProjectRoot",
              "project shared-lib overrides engine shared-lib");
        CHECK(definition.roots[0].params_json == "{\"a\":1,\"z\":2}",
              "root params are canonical owned JSON");
        CHECK(definition.roots[0].transform.m[3] == 4.0f &&
                  definition.roots[0].transform.m[7] == 5.0f &&
                  definition.roots[0].transform.m[11] == 6.0f,
              "root transform extracted");
        CHECK(definition.roots[0].expand && !definition.roots[0].tileset,
              "expand flag extracted");
        CHECK(definition.roots[1].tileset && !definition.roots[1].expand,
              "tileset flag extracted");
    }
    CHECK(definition.lights.size() == 1, "one light extracted");
    if (definition.lights.size() == 1) {
        CHECK(definition.lights[0].position.x == 1.0f &&
                  definition.lights[0].position.y == 2.0f &&
                  definition.lights[0].position.z == 3.0f,
              "light position extracted");
        CHECK(definition.lights[0].intensity == 2.5f &&
                  definition.lights[0].range == 42.0f,
              "light intensity and range extracted");
    }
    CHECK(definition.settings.sector_size == 32.0f &&
              definition.settings.y_min == -12.0f &&
              definition.settings.y_max == 88.0f,
          "world settings extracted");

    CHECK(definition.entities.size() == 2,
          "declarative and buildEntities records share one stream");
    if (definition.entities.size() == 2) {
        CHECK(definition.entities[0].authored_id == "static-one" &&
                  definition.entities[1].authored_id == "built-one",
              "buildEntities records append after static entities");
        CHECK(definition.entities[1].parent_authored_id == "static-one",
              "entity parent authored id extracted");
        CHECK(definition.entities[1].components_json ==
                  "{\"SeedProbe\":{\"difficulty\":3,\"seed\":77}}",
              "seed and canonical parameters are explicit build bindings");
    }
}

void test_engine_shared_fallback_and_no_entity_world() {
    Fixture fixture;
    fixture.write("engine-shared/choice.js",
                  "export const chosen = 'EngineFallback';\n");
    const fs::path path = fixture.write("Empty.js", R"JS(
import { chosen } from 'shared-lib/choice';
class EmptyWorld extends World {
  static roots = [{ module: chosen }];
  field() { throw new Error('field must not run'); }
}
)JS");

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());
    CHECK(definition.roots.size() == 1 &&
              definition.roots[0].module == "EngineFallback",
          "engine shared-lib supplies missing project module");
    CHECK(definition.entities.empty(), "worlds without entities remain valid");
}

bool definition_is_cleared(const matter::WorldDefinition& definition) {
    return definition.roots.empty() && definition.lights.empty() &&
           definition.entities.empty() &&
           definition.settings.sector_size == 16.0f &&
           definition.settings.y_min == -64.0f &&
           definition.settings.y_max == 192.0f;
}

struct ExpectedExampleRoot {
    const char* module;
    bool expand;
    bool tileset;
};

struct ExpectedExampleWorld {
    const char* name;
    std::initializer_list<ExpectedExampleRoot> roots;
    matter::Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    matter::Float3 sun_color{2.2f, 2.05f, 1.8f};
    matter::Float3 sky_color{0.38f, 0.43f, 0.52f};
    float sector_size = 16.0f;
    float y_min = -64.0f;
    float y_max = 192.0f;
};

bool nearly_equal(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

void test_example_worlds_preserve_manifest_authoring() {
    const fs::path project = fs::path("../examples/world_demo");
    const ExpectedExampleWorld worlds[] = {
        {"Demo", {{"TreeGallery", false, false}}},
        {"Meadow", {{"Meadow", true, false},
                    {"ForestFloor", false, true}}},
        {"MeadowWorld", {},
         {-0.45f, -0.80f, -0.35f}, {2.2f, 2.05f, 1.8f},
         {0.38f, 0.43f, 0.52f}, 64.0f, -16.0f, 240.0f},
        {"CornellBox", {{"CornellBox", false, false}},
         {0.1003569f, -0.9834976f, -0.1505354f}, {3.0f, 3.0f, 3.0f},
         {0.6f, 0.65f, 0.75f}},
        {"LightingGarden", {{"LightingGarden", false, false}},
         {-0.5534701f, -0.3522082f, -0.7547319f}, {0.45f, 0.24f, 0.12f},
         {0.055f, 0.075f, 0.16f}},
        {"FloorDemo", {{"FloorDemo", false, false},
                       {"ForestFloor", false, true}}},
        {"RockGallery", {{"RockGallery", true, false}}},
        {"StressForest50k", {{"StressForest50k", true, false}}},
        {"StressForest100k", {{"StressForest100k", true, false}}},
        {"StressForest200k", {{"StressForest200k", true, false}}},
        {"StressForest500k", {{"StressForest500k", true, false}}},
    };
    const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    CHECK(fs::is_directory(project / "objects"),
          "example project exposes object modules under objects/");
    CHECK(!fs::exists(project / "schemas"),
          "example project no longer exposes the legacy schemas/ directory");

    for (const ExpectedExampleWorld& expected : worlds) {
        CHECK(fs::is_regular_file(
                  project / "worlds" / (std::string(expected.name) + ".js")),
              (std::string(expected.name) + " remains a selectable identity").c_str());
        matter::WorldLoadDesc desc;
        desc.world_path =
            (project / "worlds" / (std::string(expected.name) + ".js")).string();
        desc.objects_dir = (project / "objects").string();
        desc.project_shared_lib_dir = (project / "shared-lib").string();
        desc.engine_shared_lib_dir = "../shared-lib";

        matter::WorldDefinition definition;
        matter::WorldLoadError error;
        CHECK(matter::load_world_definition(desc, definition, error),
              (std::string(expected.name) + ": " + error.message).c_str());
        CHECK(definition.roots.size() == expected.roots.size(),
              (std::string(expected.name) + " root count").c_str());
        std::size_t index = 0;
        for (const ExpectedExampleRoot& root : expected.roots) {
            CHECK(fs::is_regular_file(
                      project / "objects" / (std::string(root.module) + ".js")),
                  (std::string(root.module) + " moved under objects/").c_str());
            if (index >= definition.roots.size()) break;
            const matter::WorldRoot& actual = definition.roots[index];
            CHECK(actual.module == root.module,
                  (std::string(expected.name) + " root order/module").c_str());
            CHECK(actual.params_json == "{}",
                  (std::string(expected.name) + " root params").c_str());
            bool identity_matches = true;
            for (std::size_t element = 0; element < 16; ++element)
                identity_matches = identity_matches &&
                    nearly_equal(actual.transform.m[element], identity[element]);
            CHECK(identity_matches,
                  (std::string(expected.name) + " root transform").c_str());
            CHECK(actual.expand == root.expand && actual.tileset == root.tileset,
                  (std::string(expected.name) + " root flags").c_str());
            ++index;
        }

        CHECK(nearly_equal(definition.settings.sun_direction.x,
                           expected.sun_direction.x) &&
                  nearly_equal(definition.settings.sun_direction.y,
                               expected.sun_direction.y) &&
                  nearly_equal(definition.settings.sun_direction.z,
                               expected.sun_direction.z) &&
                  nearly_equal(definition.settings.sun_color.x,
                               expected.sun_color.x) &&
                  nearly_equal(definition.settings.sun_color.y,
                               expected.sun_color.y) &&
                  nearly_equal(definition.settings.sun_color.z,
                               expected.sun_color.z) &&
                  nearly_equal(definition.settings.sky_color.x,
                               expected.sky_color.x) &&
                  nearly_equal(definition.settings.sky_color.y,
                               expected.sky_color.y) &&
                  nearly_equal(definition.settings.sky_color.z,
                               expected.sky_color.z),
              (std::string(expected.name) + " authored lights").c_str());
        CHECK(nearly_equal(definition.settings.sector_size,
                           expected.sector_size) &&
                  nearly_equal(definition.settings.y_min, expected.y_min) &&
                  nearly_equal(definition.settings.y_max, expected.y_max),
              (std::string(expected.name) + " procedural settings").c_str());
    }
}

void test_authored_entity_override_cannot_intercept_collection() {
    Fixture fixture;
    const fs::path path = fixture.write("Override.js", R"JS(
class OverrideWorld extends World {
  entity(record) { throw new Error('authored entity override ran'); }
  buildEntities() {
    this.entity({ id: 'loader-owned', components: { Marker: true } });
  }
}
)JS");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;

    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());
    CHECK(definition.entities.size() == 1 &&
              definition.entities[0].authored_id == "loader-owned",
          "loader-controlled entity dispatch cannot be shadowed by authored prototype");
}

void test_rejects_undefined_or_non_json_owned_values() {
    Fixture fixture;
    const fs::path params_path = fixture.write("BadParams.js", R"JS(
class BadParamsWorld extends World {
  static roots = [{ module: 'Root', params: () => 1 }];
}
)JS");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(!matter::load_world_definition(fixture.desc(params_path), definition, error),
          "function-valued root params are rejected");
    CHECK(error.property_path == "roots[0].params",
          "non-JSON params report their contextual property");
    CHECK(definition_is_cleared(definition),
          "non-JSON params leave no partial definition");

    const fs::path components_path = fixture.write("BadComponents.js", R"JS(
class BadComponentsWorld extends World {
  static entities = [{ id: 'bad', components: undefined }];
}
)JS");
    CHECK(!matter::load_world_definition(fixture.desc(components_path),
                                         definition, error),
          "explicit undefined components are rejected");
    CHECK(error.property_path == "entities[0].components",
          "undefined components report their contextual property");
    CHECK(definition_is_cleared(definition),
          "undefined components leave no partial definition");
}

void test_build_entities_failures_clear_partial_definition() {
    Fixture fixture;
    const fs::path non_callable_path = fixture.write("NonCallable.js", R"JS(
class NonCallableWorld extends World {
  static roots = [{ module: 'Root' }];
  static lights = [{ position: [1, 2, 3] }];
  get buildEntities() { return 42; }
}
)JS");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(!matter::load_world_definition(fixture.desc(non_callable_path),
                                         definition, error),
          "non-callable buildEntities is rejected");
    CHECK(error.property_path == "buildEntities",
          "non-callable buildEntities reports its property");
    CHECK(definition_is_cleared(definition),
          "non-callable buildEntities clears extracted roots and lights");

    const fs::path throwing_path = fixture.write("Throwing.js", R"JS(
class ThrowingWorld extends World {
  static roots = [{ module: 'Root' }];
  static settings = { sectorSize: 64 };
  buildEntities() { throw new Error('bootstrap failed'); }
}
)JS");
    CHECK(!matter::load_world_definition(fixture.desc(throwing_path),
                                         definition, error),
          "throwing buildEntities is rejected");
    CHECK(error.property_path == "buildEntities" &&
              error.message.find("bootstrap failed") != std::string::npos,
          "buildEntities exception retains contextual diagnostics");
    CHECK(definition_is_cleared(definition),
          "throwing buildEntities clears extracted roots and settings");
}

// ---------------------------------------------------------------------------
// Fog extraction tests (Task 4: volumetrics)
// ---------------------------------------------------------------------------

void test_fog_extraction_with_authored_values() {
    Fixture fixture;
    const fs::path path = fixture.write("FogWorld.js", R"JS(
class FogWorld extends World {
  static roots = [{ module: 'Root' }];
  static fog = {
    density: 0.05,
    floor: -10.0,
    falloff: 50.0,
    color: [0.8, 0.85, 0.9],
    wind: [1.0, 0.0, 0.5],
  };
}
)JS");

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());

    const matter::FogSettings& fog = definition.settings.fog;
    CHECK(nearly_equal(fog.density, 0.05f), "fog density extracted");
    CHECK(nearly_equal(fog.floor, -10.0f), "fog floor extracted");
    CHECK(nearly_equal(fog.falloff, 50.0f), "fog falloff extracted");
    CHECK(nearly_equal(fog.color[0], 0.8f) &&
              nearly_equal(fog.color[1], 0.85f) &&
              nearly_equal(fog.color[2], 0.9f),
          "fog color extracted");
    CHECK(nearly_equal(fog.wind[0], 1.0f) &&
              nearly_equal(fog.wind[1], 0.0f) &&
              nearly_equal(fog.wind[2], 0.5f),
          "fog wind extracted");
}

void test_fog_defaults_when_absent() {
    Fixture fixture;
    const fs::path path = fixture.write("NoFogWorld.js", R"JS(
class NoFogWorld extends World {
  static roots = [{ module: 'Root' }];
}
)JS");

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());

    const matter::FogSettings& fog = definition.settings.fog;
    CHECK(nearly_equal(fog.density, 0.0f), "fog density defaults to 0 (no fog)");
    CHECK(nearly_equal(fog.floor, 0.0f), "fog floor defaults to 0");
    CHECK(nearly_equal(fog.falloff, 30.0f), "fog falloff defaults to 30");
    CHECK(nearly_equal(fog.color[0], 0.9f) &&
              nearly_equal(fog.color[1], 0.92f) &&
              nearly_equal(fog.color[2], 0.95f),
          "fog color defaults to neutral blue-white");
    CHECK(nearly_equal(fog.wind[0], 0.0f) &&
              nearly_equal(fog.wind[1], 0.0f) &&
              nearly_equal(fog.wind[2], 0.0f),
          "fog wind defaults to zero");
}

void test_vulkan_volumetrics_settings_defaults() {
    matter::VulkanVolumetricsSettings vol{};
    CHECK(vol.enabled == false, "volumetrics disabled by default");
    CHECK(nearly_equal(vol.temporal_blend, 0.85f), "temporal_blend defaults to 0.85");
    CHECK(nearly_equal(vol.phase_g, 0.3f), "phase_g defaults to 0.3");
    CHECK(nearly_equal(vol.fog_density_mul, 1.0f), "fog_density_mul defaults to 1.0");
    CHECK(nearly_equal(vol.fog_floor_offset, 0.0f), "fog_floor_offset defaults to 0.0");
    CHECK(nearly_equal(vol.fog_falloff_mul, 1.0f), "fog_falloff_mul defaults to 1.0");
    CHECK(nearly_equal(vol.fog_color_mul[0], 1.0f) &&
              nearly_equal(vol.fog_color_mul[1], 1.0f) &&
              nearly_equal(vol.fog_color_mul[2], 1.0f),
          "fog_color_mul defaults to white");
    CHECK(nearly_equal(vol.fog_wind_mul[0], 1.0f) &&
              nearly_equal(vol.fog_wind_mul[1], 1.0f) &&
              nearly_equal(vol.fog_wind_mul[2], 1.0f),
          "fog_wind_mul defaults to identity");
    CHECK(nearly_equal(vol.vol_debug_view, 0.0f), "vol_debug_view defaults to 0.0");
}

} // namespace

int main() {
    test_project_layout_derives_runtime_paths();
    test_example_worlds_preserve_manifest_authoring();
    test_rejects_non_world_base_with_location_and_property();
    test_extracts_statics_without_calling_field_and_uses_project_override();
    test_engine_shared_fallback_and_no_entity_world();
    test_authored_entity_override_cannot_intercept_collection();
    test_rejects_undefined_or_non_json_owned_values();
    test_build_entities_failures_clear_partial_definition();
    test_fog_extraction_with_authored_values();
    test_fog_defaults_when_absent();
    test_vulkan_volumetrics_settings_defaults();
    return check_summary();
}
