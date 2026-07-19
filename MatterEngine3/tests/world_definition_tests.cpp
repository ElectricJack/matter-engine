// Phase 4 Task 1: engine-owned World JavaScript statics contract.
#include "check.h"
#include "../src/script/world_definition_loader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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

} // namespace

int main() {
    test_rejects_non_world_base_with_location_and_property();
    test_extracts_statics_without_calling_field_and_uses_project_override();
    test_engine_shared_fallback_and_no_entity_world();
    test_authored_entity_override_cannot_intercept_collection();
    test_rejects_undefined_or_non_json_owned_values();
    test_build_entities_failures_clear_partial_definition();
    return check_summary();
}
