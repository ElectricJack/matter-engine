// Phase 4 Task 1: engine-owned World JavaScript statics contract.
#include "check.h"
#include "../src/provider/local_provider.h"
#include "../src/script/world_definition_loader.h"
#include "../src/detail_bake_plan.h"
#include "../src/tileset_slot_allocator.h"

extern "C" {
#include "material_registry.h"
}

#include <algorithm>
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

// Phase 1 (repo-layout-and-cache-consolidation plan) cache-leak fix:
// LocalProviderConfig::for_project() must never hand back a relative
// cache_root. Before the fix, cache_root was built by appending
// ".cache"/<world_name> directly onto whatever project_dir string the caller
// passed in (project / ".cache" / world_name), so a relative project_dir
// (exactly what api_tests.cpp's "../projects/primitive_demo"
// and world_stream_tests.cpp's "../MatterEngine3/tests/fixtures/world_stream"
// pass) produced a relative cache_root. Every downstream writer that
// composes an output path directly from cache_root (PartStore, resolve_cache,
// live_edit_prod::ProdBaker/ProdFlattener, WorldTracer -- all in
// matter_engine.cpp) then wrote wherever the *process's* cwd happened to be
// at the moment of each write, not next to the project -- the exact bug
// behind the stray parts/, imposters/, and libs/MatterSurfaceLib/parts/
// directories this phase cleans up.
void test_relative_project_dir_yields_absolute_cache_root() {
    const std::string relative_project = "world_definition_relcache_fixture";
    const fs::path expected_project_abs = fs::absolute(fs::path(relative_project));

    auto cfg = viewer::LocalProviderConfig::for_project(
        relative_project, "Demo", "");

    CHECK(fs::path(cfg.project_dir).is_absolute(),
          "for_project() absolutizes a relative project_dir");
    CHECK(fs::path(cfg.cache_root).is_absolute(),
          "cache_root is never a bare relative path, even when project_dir is relative "
          "(this is the assertion that fails without the Phase 1 fix)");
    CHECK(cfg.cache_root == (expected_project_abs / ".cache" / "Demo").string(),
          "relative project_dir resolves through std::filesystem::absolute() before "
          "'.cache/<world_name>' is appended, matching the co-located convention");

    // An already-absolute project_dir must resolve to the identical cache_root
    // (absolutizing is idempotent, not a second, divergent transform).
    auto cfg_from_absolute = viewer::LocalProviderConfig::for_project(
        expected_project_abs.string(), "Demo", "");
    CHECK(cfg_from_absolute.cache_root == cfg.cache_root,
          "an already-absolute project_dir produces the identical cache_root");
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
    matter::Float3 translation{0.0f, 0.0f, 0.0f};
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
    const fs::path project = fs::path("../../projects/world_demo");
    const ExpectedExampleWorld worlds[] = {
        {"Demo", {{"TreeGallery", false, false},
                  {"ChimneySmoke", false, false, {5.0f, 6.0f, 0.0f}},
                  {"WaterfallMist", false, false, {-8.0f, 0.0f, 10.0f}}}},
        {"Meadow", {{"Meadow", true, false},
                    {"ForestFloor", false, true}}},
        {"CornellBox", {{"CornellBox", false, false}},
         {0.1003569f, -0.9834976f, -0.1505354f}, {3.0f, 3.0f, 3.0f},
         {0.6f, 0.65f, 0.75f}},
        {"LightingGarden", {{"LightingGarden", false, false}},
         {-0.5534701f, -0.3522082f, -0.7547319f}, {0.45f, 0.24f, 0.12f},
         {0.055f, 0.075f, 0.16f}},
        {"FloorDemo", {{"FloorDemo", false, false},
                       {"ForestFloor", false, true}}},
        {"RockGallery", {{"RockGallery", true, false}}},
        // The StressForest{50k,100k,200k,500k} entries died with the worlds
        // themselves ("Remove outdated worlds", 530b7a11): this table checks
        // that SHIPPED example worlds keep their manifest authoring, so a
        // deleted world has nothing left to preserve.
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
            const float expected_transform[16] = {
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                root.translation.x, root.translation.y, root.translation.z, 1,
            };
            bool transform_matches = true;
            for (std::size_t element = 0; element < 16; ++element)
                transform_matches = transform_matches &&
                    nearly_equal(actual.transform.m[element],
                                 expected_transform[element]);
            CHECK(transform_matches,
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
    minHeight: 4.0,
    maxHeight: 64.0,
    noiseScale: 0.002,
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
    CHECK(fog.height_layer &&
              nearly_equal(fog.min_height, 4.0f) &&
              nearly_equal(fog.max_height, 64.0f) &&
              nearly_equal(fog.noise_scale, 0.002f),
          "fog bounded height layer extracted");
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
    CHECK(!fog.height_layer &&
              nearly_equal(fog.noise_scale, 0.0018f),
          "fog bounded height layer defaults off");
    CHECK(nearly_equal(fog.color[0], 0.9f) &&
              nearly_equal(fog.color[1], 0.92f) &&
              nearly_equal(fog.color[2], 0.95f),
          "fog color defaults to neutral blue-white");
    CHECK(nearly_equal(fog.wind[0], 0.0f) &&
              nearly_equal(fog.wind[1], 0.0f) &&
              nearly_equal(fog.wind[2], 0.0f),
          "fog wind defaults to zero");
}

void test_streaming_ring_extraction() {
    Fixture fixture;
    const fs::path path = fixture.write("StreamingWorld.js", R"JS(
class StreamingWorld extends World {
  static roots = [{ module: 'Root' }];
  static streaming = {
    rings: [
      { radius: 128, rung: 2 },
      { radius: 320, rung: 1 },
      { radius: 4800, rung: 0 },
    ],
  };
}
)JS");

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());

    const auto& rings = definition.settings.streaming_rings;
    CHECK(rings.size() == 3, "world streaming rings extracted");
    CHECK(rings.size() == 3 &&
              nearly_equal(rings[0].radius, 128.0f) && rings[0].rung == 2 &&
              nearly_equal(rings[1].radius, 320.0f) && rings[1].rung == 1 &&
              nearly_equal(rings[2].radius, 4800.0f) && rings[2].rung == 0,
          "world streaming ring values preserve authored order");
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

// ---------------------------------------------------------------------------
// World.props — script-declared runtime tunables (property system S9)
// ---------------------------------------------------------------------------

// The fixture world every props test below loads: one field of each kind the
// v1 authoring surface accepts, plus a buildEntities() that reads them back
// through getProp so the definition-time read path is covered too.
constexpr const char* kPropsWorldSource = R"JS(
class PropsWorld extends World {
  static props = {
    spinSpeed: { default: 1.2, min: 0, max: 10, step: 0.1,
                 doc: 'Rotations per second', label: 'Spin speed', units: 'rps' },
    creaky:    { default: true },
    banner:    { default: 'windmill' },
    season:    { default: 1, enum: ['spring', 'summer', 'winter'] },
  };
  buildEntities() {
    this.entity({
      id: 'readback',
      components: {
        Probe: {
          spin: getProp('spinSpeed'),
          creaky: getProp('creaky'),
          banner: getProp('banner'),
          season: getProp('season'),
        },
      },
    });
  }
}
)JS";

// Loads a one-property world and returns the error for the failure cases.
bool load_props_world(Fixture& fixture, const std::string& body,
                      matter::WorldDefinition& definition,
                      matter::WorldLoadError& error) {
    const fs::path path = fixture.write(
        "PropsWorld.js",
        "class PropsWorld extends World {\n  static props = " + body + ";\n}\n");
    return matter::load_world_definition(fixture.desc(path), definition, error);
}

void test_props_extraction() {
    Fixture fixture;
    const fs::path path = fixture.write("PropsWorld.js", kPropsWorldSource);

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());

    CHECK(definition.props.size() == 4, "every declared property is recorded");
    // Declaration order, which is the order the panel draws them in.
    CHECK(definition.props[0].name == "spinSpeed" &&
              definition.props[1].name == "creaky" &&
              definition.props[2].name == "banner" &&
              definition.props[3].name == "season",
          "props preserve declaration order");

    const matter::WorldPropSpec& spin = definition.props[0];
    CHECK(spin.kind == matter::WorldPropSpec::Kind::Float,
          "a numeric default without enum labels is a Float");
    CHECK(nearly_equal(static_cast<float>(spin.number_default), 1.2f),
          "float default");
    CHECK(spin.has_range && nearly_equal(spin.min, 0.0f) &&
              nearly_equal(spin.max, 10.0f),
          "min/max declared together become a range");
    CHECK(nearly_equal(spin.step, 0.1f), "step carried through");
    CHECK(spin.label == "Spin speed" && spin.doc == "Rotations per second" &&
              spin.units == "rps",
          "label/doc/units carried through");

    CHECK(definition.props[1].kind == matter::WorldPropSpec::Kind::Bool &&
              definition.props[1].bool_default,
          "a boolean default is a Bool");
    CHECK(definition.props[2].kind == matter::WorldPropSpec::Kind::String &&
              definition.props[2].string_default == "windmill",
          "a string default is a String");

    const matter::WorldPropSpec& season = definition.props[3];
    CHECK(season.kind == matter::WorldPropSpec::Kind::Enum,
          "enum labels turn a numeric default into an Enum");
    CHECK(season.enum_labels.size() == 3 && season.enum_labels[0] == "spring" &&
              season.enum_labels[2] == "winter",
          "enum labels recorded in order");
    CHECK(static_cast<int>(season.number_default) == 1,
          "an enum default is the label index");
    CHECK(!season.has_range, "a non-float property carries no range");

    // getProp, from buildEntities: declared defaults, and the enum as its LABEL.
    CHECK(definition.entities.size() == 1, "buildEntities ran");
    const std::string& components = definition.entities[0].components_json;
    CHECK(components.find("\"spin\":1.2") != std::string::npos,
          "getProp returns the declared float default");
    CHECK(components.find("\"creaky\":true") != std::string::npos,
          "getProp returns the declared bool default");
    CHECK(components.find("\"banner\":\"windmill\"") != std::string::npos,
          "getProp returns the declared string default");
    CHECK(components.find("\"season\":\"summer\"") != std::string::npos,
          "getProp returns an enum as its label, not its index");
}

void test_props_absent_and_empty() {
    Fixture fixture;
    const fs::path path = fixture.write("Plain.js", R"JS(
class Plain extends World {
  static roots = [{ module: 'Terrain' }];
}
)JS");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());
    CHECK(definition.props.empty(), "a world without props declares none");

    matter::WorldDefinition empty_def;
    matter::WorldLoadError empty_error;
    CHECK(load_props_world(fixture, "{}", empty_def, empty_error),
          "an empty props block is legal");
    CHECK(empty_def.props.empty(), "an empty props block yields no specs");
}

// Each rejection names the offending property in WorldLoadError::property_path,
// the same contract the rest of the loader's validation follows.
void test_props_validation_paths() {
    struct Case {
        const char* body;
        const char* property_path;
        const char* what;
    };
    const Case cases[] = {
        {"[1, 2]", "props", "props must be an object, not an array"},
        {"{ spin: 1.2 }", "props.spin", "a bare value is not a spec object"},
        {"{ spin: { min: 0, max: 1 } }", "props.spin.default",
         "a missing default is rejected"},
        {"{ spin: { default: 1, wobble: 3 } }", "props.spin",
         "an unknown spec key is rejected"},
        {"{ spin: { default: [1, 2] } }", "props.spin.default",
         "an array default has no kind"},
        {"{ spin: { default: null } }", "props.spin.default",
         "a null default has no kind"},
        {"{ spin: { default: 1, min: 0 } }", "props.spin",
         "min without max is rejected"},
        {"{ spin: { default: 1, max: 4 } }", "props.spin",
         "max without min is rejected"},
        {"{ spin: { default: 1, min: 4, max: 0 } }", "props.spin",
         "an inverted range is rejected"},
        {"{ spin: { default: 9, min: 0, max: 4 } }", "props.spin.default",
         "a default outside the range is rejected"},
        {"{ spin: { default: 1, doc: 7 } }", "props.spin",
         "a non-string doc is rejected"},
        {"{ mode: { default: 0, enum: [] } }", "props.mode.enum",
         "an empty enum label list is rejected"},
        {"{ mode: { default: 0, enum: 'spring' } }", "props.mode.enum",
         "a non-array enum is rejected"},
        {"{ mode: { default: 0, enum: ['a', 7] } }", "props.mode.enum[1]",
         "a non-string enum label is rejected"},
        {"{ mode: { default: 5, enum: ['a', 'b'] } }", "props.mode.default",
         "an out-of-range enum index is rejected"},
        {"{ mode: { default: 0.5, enum: ['a', 'b'] } }", "props.mode.default",
         "a fractional enum index is rejected"},
        {"{ mode: { default: 'a', enum: ['a', 'b'] } }", "props.mode.default",
         "an enum default must be an index, not a label"},
        {"{ mode: { default: true, enum: ['a', 'b'] } }", "props.mode.default",
         "a boolean enum default is rejected"},
    };

    for (const Case& item : cases) {
        Fixture fixture;
        matter::WorldDefinition definition;
        matter::WorldLoadError error;
        const bool loaded = load_props_world(fixture, item.body, definition, error);
        CHECK(!loaded, item.what);
        CHECK(error.property_path == item.property_path,
              (std::string(item.what) + " -> property_path '" +
               item.property_path + "' (got '" + error.property_path + "')")
                  .c_str());
        CHECK(!error.message.empty(), "a rejection carries a message");
        CHECK(definition.props.empty(),
              "a rejected props block leaves no partial specs behind");
    }
}

void test_props_getprop_diagnostics() {
    Fixture fixture;
    // Unknown name from buildEntities.
    fs::path path = fixture.write("Unknown.js", R"JS(
class Unknown extends World {
  static props = { spin: { default: 1 } };
  buildEntities() { getProp('nope'); }
}
)JS");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(!matter::load_world_definition(fixture.desc(path), definition, error),
          "getProp on an undeclared name fails the load");
    CHECK(error.property_path == "buildEntities",
          "an unknown getProp is reported against buildEntities");
    CHECK(error.message.find("static props") != std::string::npos,
          "the message names the props block");

    // Called while the class statics evaluate — the props block is itself one.
    path = fixture.write("TooEarly.js", R"JS(
class TooEarly extends World {
  static props = { spin: { default: 1 } };
  static roots = [{ module: 'Terrain', params: { s: getProp('spin') } }];
}
)JS");
    matter::WorldDefinition early_def;
    matter::WorldLoadError early_error;
    CHECK(!matter::load_world_definition(fixture.desc(path), early_def, early_error),
          "getProp during class-static evaluation fails the load");
    CHECK(early_error.message.find("buildEntities") != std::string::npos,
          "the too-early message points at buildEntities");
}

// ---------------------------------------------------------------------------
// defineMaterial + automated detail bakes + slot allocator (chart-VT Phase 3)
// ---------------------------------------------------------------------------

void test_define_material_round_trip() {
    Fixture fixture;
    const fs::path path = fixture.write("Materials.js", R"JS(
const ROCK = defineMaterial('AlpineRock', {
  albedo: [0.42, 0.40, 0.38], roughness: 0.9, metallic: 0.0,
  detail: 'AlpineRockDetail', detailDensity: 24,
});
const SNOW = defineMaterial('AlpineSnow', {
  albedo: [0.93, 0.95, 1.0], roughness: 0.55, clearcoat: 0.4,
  detail: 'AlpineSnowDetail',
});
class MaterialWorld extends World {
  static roots = [{ module: 'Terrain', params: { rock: ROCK, snow: SNOW } }];
}
)JS");

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(path), definition, error),
          error.message.c_str());

    const int base = MaterialRegistryStaticCount();
    CHECK(definition.materials.size() == 2, "both declared materials recorded");
    if (definition.materials.size() != 2) return;
    CHECK(definition.materials[0].name == "AlpineRock" &&
              definition.materials[1].name == "AlpineSnow",
          "materials retain declaration order");
    CHECK(definition.materials[0].index == base &&
              definition.materials[1].index == base + 1,
          "handles are appended after the frozen builtin table");
    CHECK(MaterialRegistryCount() == base + 2,
          "dynamic entries extend the registry count the GPU pack iterates");
    CHECK(MaterialRegistryDynamicCount() == 2, "dynamic count tracks declarations");

    // The handle the script saw is the id the renderer will index.
    CHECK(definition.roots.size() == 1 &&
              definition.roots[0].params_json ==
                  "{\"rock\":" + std::to_string(base) +
                      ",\"snow\":" + std::to_string(base + 1) + "}",
          "handles are plain ints usable in root params");

    const MaterialDef* rock = MaterialRegistryGet(base);
    CHECK(nearly_equal(rock->albedo[0], 0.42f) &&
              nearly_equal(rock->albedo[1], 0.40f) &&
              nearly_equal(rock->albedo[2], 0.38f) &&
              nearly_equal(rock->roughness, 0.9f) &&
              nearly_equal(rock->metallic, 0.0f),
          "authored spec fields reach the MaterialDef");
    CHECK(nearly_equal(rock->opacity, 1.0f) &&
              nearly_equal(rock->specularStrength, 1.0f) &&
              nearly_equal(rock->specularTint[0], 1.0f) &&
              nearly_equal(rock->shadowOpacity, 1.0f) &&
              rock->groundTilesetSlot == -1 && rock->groundMacroSlot == -1,
          "unspecified fields take the builtin-matching defaults");
    const MaterialDef* snow = MaterialRegistryGet(base + 1);
    CHECK(nearly_equal(snow->clearcoat, 0.4f), "advanced fields are mappable");
    CHECK(rock->mergeGroup != snow->mergeGroup && rock->mergeGroup >= 1000 &&
              snow->mergeGroup >= 1000,
          "dynamic materials get distinct merge groups above the builtin range");

    CHECK(MaterialRegistryFindByName("AlpineRock") == base &&
              MaterialRegistryNameOf(base) == std::string("AlpineRock"),
          "name lookup round-trips");
    CHECK(MaterialRegistryFindByName("NotDeclared") == -1,
          "unknown names do not resolve");

    CHECK(definition.materials[0].detail_module == "AlpineRockDetail" &&
              definition.materials[0].detail_density == 24,
          "detail module and density are retained for the bake scheduler");
    CHECK(definition.materials[1].detail_module == "AlpineSnowDetail" &&
              definition.materials[1].detail_density == 0,
          "an omitted detailDensity keeps the tileset module's own density");
}

void test_define_material_reset_between_worlds() {
    Fixture fixture;
    const fs::path first = fixture.write("First.js", R"JS(
defineMaterial('OnlyFirst', { roughness: 0.3 });
class FirstWorld extends World { static roots = []; }
)JS");
    const fs::path second = fixture.write("Second.js", R"JS(
const A = defineMaterial('SecondA', { roughness: 0.4 });
class SecondWorld extends World { static roots = [{ module: 'X', params: { a: A } }]; }
)JS");

    const int base = MaterialRegistryStaticCount();
    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(first), definition, error),
          error.message.c_str());
    CHECK(MaterialRegistryFindByName("OnlyFirst") == base,
          "first world's material occupies the first dynamic index");

    CHECK(matter::load_world_definition(fixture.desc(second), definition, error),
          error.message.c_str());
    CHECK(MaterialRegistryFindByName("OnlyFirst") == -1,
          "loading a world drops the previous world's dynamic entries");
    CHECK(MaterialRegistryFindByName("SecondA") == base,
          "the freed index is reused, so handles stay deterministic");
    CHECK(MaterialRegistryDynamicCount() == 1, "no leakage across loads");

    // Same world twice must produce identical handles (idempotent load).
    CHECK(matter::load_world_definition(fixture.desc(second), definition, error),
          error.message.c_str());
    CHECK(definition.materials.size() == 1 &&
              definition.materials[0].index == base,
          "re-loading one world yields the same handle");
}

void test_define_material_name_collision_rules() {
    Fixture fixture;
    const int base = MaterialRegistryStaticCount();

    // Identical re-definition is the shared-lib import case: same handle, one
    // record, no error.
    const fs::path same = fixture.write("Same.js", R"JS(
const A = defineMaterial('Shared', { roughness: 0.5, albedo: [0.1, 0.2, 0.3] });
const B = defineMaterial('Shared', { roughness: 0.5, albedo: [0.1, 0.2, 0.3] });
class SameWorld extends World { static roots = [{ module: 'X', params: { a: A, b: B } }]; }
)JS");
    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(same), definition, error),
          error.message.c_str());
    CHECK(definition.materials.size() == 1 &&
              definition.materials[0].index == base,
          "an identical re-definition collapses to one material");
    CHECK(definition.roots.size() == 1 &&
              definition.roots[0].params_json ==
                  "{\"a\":" + std::to_string(base) + ",\"b\":" +
                      std::to_string(base) + "}",
          "both call sites receive the same handle");

    const fs::path differ = fixture.write("Differ.js", R"JS(
defineMaterial('Shared', { roughness: 0.5 });
defineMaterial('Shared', { roughness: 0.9 });
class DifferWorld extends World { static roots = []; }
)JS");
    CHECK(!matter::load_world_definition(fixture.desc(differ), definition, error),
          "a mismatched re-definition is rejected");
    CHECK(error.property_path == "source" &&
              error.message.find("different spec") != std::string::npos,
          "the mismatch diagnostic names the cause");
}

void test_define_material_rejects_bad_specs() {
    Fixture fixture;
    matter::WorldDefinition definition;
    matter::WorldLoadError error;

    const fs::path typo = fixture.write("Typo.js", R"JS(
defineMaterial('Typo', { roughnes: 0.5 });
class TypoWorld extends World { static roots = []; }
)JS");
    CHECK(!matter::load_world_definition(fixture.desc(typo), definition, error),
          "a misspelled spec field is rejected rather than silently defaulted");
    CHECK(error.message.find("unknown spec field 'roughnes'") != std::string::npos,
          "the unknown-field diagnostic names the field");

    const fs::path density = fixture.write("Density.js", R"JS(
defineMaterial('NoDetail', { detailDensity: 16 });
class DensityWorld extends World { static roots = []; }
)JS");
    CHECK(!matter::load_world_definition(fixture.desc(density), definition, error),
          "detailDensity without a detail tileset is rejected");

    const fs::path late = fixture.write("Late.js", R"JS(
class LateWorld extends World {
  static roots = [];
  buildEntities() { defineMaterial('TooLate', {}); }
}
)JS");
    CHECK(!matter::load_world_definition(fixture.desc(late), definition, error),
          "declaring a material after roots resolve is diagnosed");
    CHECK(error.property_path == "buildEntities" &&
              error.message.find("before World.roots is read") != std::string::npos,
          "the too-late diagnostic explains the ordering requirement");
}

void test_detail_bake_plan_ordering_and_merging() {
    // The deprecated alias alone must produce exactly what the hardcoded path
    // produced: one request for the root module bound to material 16.
    std::vector<tileset::DetailBakeRoot> roots{{"ForestFloor", "{}"}};
    auto plan = tileset::plan_detail_bakes(roots, {});
    CHECK(plan.size() == 1 && plan[0].module == "ForestFloor" &&
              plan[0].materials.size() == 1 && plan[0].materials[0] == 16 &&
              plan[0].from_tileset_root && plan[0].texels_per_meter == 0,
          "a `tileset: true` root still binds material 16 and nothing else");

    // Declared materials append after the roots, in declaration order.
    std::vector<matter::WorldMaterial> materials{
        {"Rock", 30, "RockDetail", 0},
        {"Snow", 31, "SnowDetail", 24},
        {"Scree", 32, "RockDetail", 0},     // shares Rock's detail scene
        {"Plain", 33, "", 0},               // no detail: never scheduled
        {"Dense", 34, "RockDetail", 48},    // same module, different density
    };
    plan = tileset::plan_detail_bakes(roots, materials);
    CHECK(plan.size() == 4, "one request per distinct (module, params, density)");
    if (plan.size() != 4) return;
    CHECK(plan[0].module == "ForestFloor" && plan[0].materials[0] == 16,
          "deprecated roots stay first so existing slot assignment is unchanged");
    CHECK(plan[1].module == "RockDetail" && plan[1].materials.size() == 2 &&
              plan[1].materials[0] == 30 && plan[1].materials[1] == 32,
          "materials sharing a detail scene share one bake and one slot");
    CHECK(plan[2].module == "SnowDetail" && plan[2].texels_per_meter == 24 &&
              plan[2].materials.size() == 1 && plan[2].materials[0] == 31,
          "detailDensity rides along with its request");
    CHECK(plan[3].module == "RockDetail" && plan[3].texels_per_meter == 48 &&
              plan[3].materials[0] == 34,
          "a different density is a different atlas, not a merge");
    for (const auto& request : plan)
        CHECK(request.materials[0] != 33, "a material without detail is not scheduled");
}

void test_slot_allocator_eviction_order() {
    tileset::SlotAllocator allocator(3);
    CHECK(allocator.capacity() == 3 && allocator.size() == 0,
          "a fresh allocator holds nothing");

    const auto a = allocator.acquire(0xAAu);
    const auto b = allocator.acquire(0xBBu);
    const auto c = allocator.acquire(0xCCu);
    CHECK(a.slot == 0 && b.slot == 1 && c.slot == 2,
          "free slots are handed out lowest-index first");
    CHECK(!a.evicted && !b.evicted && !c.evicted && !a.reused,
          "filling the pool evicts nothing");

    const auto again = allocator.acquire(0xAAu);
    CHECK(again.slot == 0 && again.reused && !again.evicted,
          "a resident key keeps its slot and is not re-baked");

    // A is now most-recently-used, so B is the victim.
    const auto d = allocator.acquire(0xDDu);
    CHECK(d.slot == 1 && d.evicted && d.evicted_key == 0xBBu &&
              d.evicted_slot == 1,
          "the least-recently-acquired atlas is displaced");
    CHECK(allocator.find(0xBBu) == -1 && allocator.find(0xAAu) == 0 &&
              allocator.find(0xDDu) == 1,
          "the evicted key is no longer resident");

    const std::vector<uint64_t> mru = allocator.keys_mru_first();
    CHECK(mru.size() == 3 && mru[0] == 0xDDu && mru[1] == 0xAAu &&
              mru[2] == 0xCCu,
          "recency order tracks acquire and reuse alike");

    allocator.touch(0xCCu);
    const auto e = allocator.acquire(0xEEu);
    CHECK(e.evicted && e.evicted_key == 0xAAu,
          "touch() promotes a key out of the victim position");

    allocator.reset();
    CHECK(allocator.size() == 0 && allocator.find(0xCCu) == -1,
          "reset empties the pool");

    CHECK(tileset::kSlotAllocatorCapacity == tileset::kMaxTilesetSlots,
          "the pool size is the renderer's slot count, not a second literal");
}

// The bind/evict bookkeeping the provider applies to the material registry:
// what a headless run can assert about "would bake + bind" without a GPU.
void test_slot_binder_reports_displaced_materials() {
    tileset::DetailSlotBinder binder(2);

    const auto first = binder.acquire(0x11u);
    CHECK(first.slot == 0 && !first.evicted, "first atlas takes slot 0");
    binder.bind(0x11u, {16});                       // deprecated root -> DIRT

    const auto second = binder.acquire(0x22u);
    CHECK(second.slot == 1 && !second.evicted, "second atlas takes slot 1");
    binder.bind(0x22u, {30, 32});                   // a shared detail scene

    // Pool full. The next atlas displaces the least-recently-acquired one and
    // must name its materials so they can fall back to scalar albedo.
    const auto third = binder.acquire(0x33u);
    CHECK(third.slot == 0 && third.evicted && third.evicted_key == 0x11u,
          "the LRU atlas is the victim");
    CHECK(third.unbound.size() == 1 && third.unbound[0] == 16,
          "eviction names exactly the displaced materials");
    binder.bind(0x33u, {31});

    // Re-acquiring a resident key rebinds nothing and evicts nothing.
    const auto again = binder.acquire(0x22u);
    CHECK(again.slot == 1 && again.reused && !again.evicted &&
              again.unbound.empty(),
          "a cached atlas is neither re-baked nor re-slotted");

    // A reservation whose atlas never loaded (headless cache miss) keeps its
    // slot but binds nothing, so reset() must not report its materials.
    const auto missed = binder.acquire(0x44u);
    CHECK(missed.evicted && missed.unbound.size() == 1 &&
              missed.unbound[0] == 31,
          "the displaced material is reported before the new atlas loads");
    binder.forget(0x44u);

    std::vector<int> released = binder.reset();
    std::sort(released.begin(), released.end());
    CHECK(released.size() == 2 && released[0] == 30 && released[1] == 32,
          "reset returns every still-bound material and nothing else");
    CHECK(binder.allocator().size() == 0, "reset empties the pool");
}

} // namespace

int main() {
    test_project_layout_derives_runtime_paths();
    test_relative_project_dir_yields_absolute_cache_root();
    test_example_worlds_preserve_manifest_authoring();
    test_rejects_non_world_base_with_location_and_property();
    test_extracts_statics_without_calling_field_and_uses_project_override();
    test_engine_shared_fallback_and_no_entity_world();
    test_authored_entity_override_cannot_intercept_collection();
    test_rejects_undefined_or_non_json_owned_values();
    test_build_entities_failures_clear_partial_definition();
    test_fog_extraction_with_authored_values();
    test_fog_defaults_when_absent();
    test_streaming_ring_extraction();
    test_vulkan_volumetrics_settings_defaults();
    test_props_extraction();
    test_props_absent_and_empty();
    test_props_validation_paths();
    test_props_getprop_diagnostics();
    test_define_material_round_trip();
    test_define_material_reset_between_worlds();
    test_define_material_name_collision_rules();
    test_define_material_rejects_bad_specs();
    test_detail_bake_plan_ordering_and_merging();
    test_slot_allocator_eviction_order();
    test_slot_binder_reports_displaced_materials();
    return check_summary();
}
