// Phase 4 Task 13 — PhysicsPlayground world and closure.
//
// Loads the PhysicsPlayground world definition (declarative static roots +
// static entities, plus procedural buildEntities()) and verifies the loader
// merges both authoring styles into a single entity stream, matching the
// authored MatterEngine3/examples/world_demo/{objects,worlds}/*.js sources.
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
        root = fs::temp_directory_path() /
               ("matter-physics-playground-" + std::to_string(
                   std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "objects");
        fs::create_directories(root / "worlds");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    fs::path write(const fs::path& relative, const std::string& content) {
        fs::path p = root / relative;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
        return p;
    }

    matter::WorldLoadDesc desc(const fs::path& world_path) const {
        matter::WorldLoadDesc d;
        d.world_path = world_path.string();
        d.objects_dir = (root / "objects").string();
        d.world_seed = 42;
        return d;
    }
};

// PhysicsPlayground.js and its two object dependencies, inlined so the test
// is hermetic (no dependency on example-tree layout at test run time). These
// mirror MatterEngine3/examples/world_demo/{objects,worlds}/*.js exactly.

const char* kPlaygroundFloorJs = R"JS(
class PlaygroundFloor extends Part {
  build(p) {
    this.fill(MAT.stone);
    const S = 20.0;
    this.beginShape(SHAPE.triangles);
      this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);
      this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);
    this.endShape();
  }
}
)JS";

const char* kCrateJs = R"JS(
class Crate extends Part {
  build(p) {
    this.fill(MAT.dirt);
    this.box(0, 0.5, 0, 1, 1, 1);
  }
}
)JS";

const char* kPhysicsPlaygroundJs = R"JS(
class PhysicsPlayground extends World {
  static roots = [
    { module: "PlaygroundFloor", transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
  ];

  static entities = [
    {
      id: "floor-body",
      name: "Floor Body",
      components: {
        LocalTransform: { translation: [0, 0, 0] },
        RigidBody: { type: "static" },
        BoxCollider: { halfExtents: [20, 0.1, 20] },
      },
    },
  ];

  buildEntities() {
    for (let i = 0; i < 5; ++i) {
      this.entity({
        id: "crate-" + i,
        name: "Crate " + i,
        components: {
          LocalTransform: { translation: [i * 2 - 4, 5 + i * 2, 0] },
          PartInstance: { part: "Crate" },
          RigidBody: { type: "dynamic" },
          BoxCollider: { halfExtents: [0.5, 0.5, 0.5] },
        },
      });
    }
  }
}
)JS";

void test_physics_playground_loads_and_merges_entity_streams() {
    Fixture fixture;
    fixture.write("objects/PlaygroundFloor.js", kPlaygroundFloorJs);
    fixture.write("objects/Crate.js", kCrateJs);
    const fs::path world_path =
        fixture.write("worlds/PhysicsPlayground.js", kPhysicsPlaygroundJs);

    matter::WorldDefinition definition;
    matter::WorldLoadError error;
    CHECK(matter::load_world_definition(fixture.desc(world_path), definition, error),
          error.message.c_str());

    // 1 + 2: world loads and floor root is present.
    CHECK(definition.roots.size() == 1, "PhysicsPlayground has one root");
    if (!definition.roots.empty()) {
        CHECK(definition.roots[0].module == "PlaygroundFloor",
              "root[0] references the PlaygroundFloor part module");
    }

    // 4: declarative + procedural entities merge into one stream (1 + 5 = 6).
    CHECK(definition.entities.size() == 6,
          "one static entity plus five procedural crates");

    // 3: static entity "floor-body" is present with the correct authored id.
    CHECK(!definition.entities.empty() &&
              definition.entities[0].authored_id == "floor-body",
          "static entity floor-body is first in the merged stream");
    if (!definition.entities.empty()) {
        CHECK(definition.entities[0].components_json ==
                  "{\"BoxCollider\":{\"halfExtents\":[20,0.1,20]},"
                  "\"LocalTransform\":{\"translation\":[0,0,0]},"
                  "\"RigidBody\":{\"type\":\"static\"}}",
              "floor-body components carry LocalTransform/RigidBody/BoxCollider");
    }

    // 5: procedural entities have sequential ids "crate-0".."crate-4".
    for (int i = 0; i < 5; ++i) {
        const std::size_t index = static_cast<std::size_t>(i) + 1;
        const std::string expected_id = "crate-" + std::to_string(i);
        CHECK(index < definition.entities.size() &&
                  definition.entities[index].authored_id == expected_id,
              (expected_id + " appears in sequential order after floor-body").c_str());
    }

    // 6: procedural crate components include the expected component keys.
    if (definition.entities.size() == 6) {
        const std::string& crate0_json = definition.entities[1].components_json;
        CHECK(crate0_json.find("\"RigidBody\"") != std::string::npos,
              "crate-0 components include RigidBody");
        CHECK(crate0_json.find("\"BoxCollider\"") != std::string::npos,
              "crate-0 components include BoxCollider");
        CHECK(crate0_json.find("\"PartInstance\"") != std::string::npos,
              "crate-0 components include PartInstance");
        CHECK(crate0_json.find("\"LocalTransform\"") != std::string::npos,
              "crate-0 components include LocalTransform");
        CHECK(crate0_json.find("\"part\":\"Crate\"") != std::string::npos,
              "crate-0 PartInstance references the Crate part module");
        CHECK(crate0_json.find("\"type\":\"dynamic\"") != std::string::npos,
              "crate-0 RigidBody type is dynamic");
    }
}

} // namespace

int main() {
    test_physics_playground_loads_and_merges_entity_streams();
    return check_summary();
}
