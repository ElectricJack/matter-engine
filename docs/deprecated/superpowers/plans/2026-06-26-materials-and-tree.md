# Materials + Instanced-Leaf Tree Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the placeholder glass-sphere world with a proper material palette and a MatterEngine2-style L-system tree whose foliage is scattered individual instanced `Leaf` parts.

**Architecture:** Append three materials to the single-source-of-truth registry and fix the JS `MAT` map. Build the missing child-instance *emission* bridge: a `placeChild(module)` DSL binding that records a `ChildInstance` at the current matrix-stack transform, child module-name→hash threaded into `build()`, and `save_v2` writing the accumulated table. Author `Leaf`/`Tree` schemas on top. Finally, expand the viewer's compose path to emit own-geometry plus recursively-placed children into the TLAS.

**Tech Stack:** C++17, QuickJS-ng (embedded JS), raylib (BLAS/TLAS types), hand-rolled `CHECK` test harness, GNU make.

**Spec:** `docs/superpowers/specs/2026-06-26-materials-and-tree-design.md`

**Working dir convention:** All commands assume repo root `/mnt/d/Shared With Desktop/AI/matter-engine-cpp` unless a `cd` is shown. Per CLAUDE.md memory: after struct/header changes, prefer `make clean && make`.

---

## File Structure

**Materials**
- `MatterSurfaceLib/src/material_registry.c` — append BARK/LEAF/DIRT entries + merge-group enums (append-only; indices 0–13 unchanged).
- `MatterEngine3/src/part_base.js.h` — rewrite the `MAT` map + add the `placeChild` method.
- `MatterEngine3/examples/world_demo/schemas/Grass.js`, `Terrain.js` — recolor to real materials.

**Emission bridge**
- `MatterEngine3/include/dsl_state.h` / `MatterEngine3/src/dsl_state.cpp` — child placement accumulator, name→hash map, `placeChild`, Matrix→row16.
- `MatterEngine3/src/dsl_bindings.cpp` — `__dsl_placeChild` binding.
- `MatterEngine3/include/script_host.h` / `MatterEngine3/src/script_host.cpp` — `bake_source` gains `child_modules`; builds name→hash map; `save_v2` writes the child table.
- `MatterEngine3/include/part_graph.h` / `MatterEngine3/src/part_graph.cpp` — `Baker::bake`/`HostBaker::bake` gain `child_modules`; `InternalNode` carries child module names.

**Tree authoring**
- `MatterEngine3/examples/world_demo/schemas/Leaf.js` (create), `Tree.js` (rewrite).

**Viewer expansion**
- `MatterEngine3/viewer/part_store.h` / `part_store.cpp` — `LoadedPart.children` kept from `load_v2`.
- `MatterEngine3/viewer/world_composer.cpp` — recursive emit own-geometry + children.

**Tests touched**
- `MatterEngine3/tests/part_graph_tests.cpp` — `FakeBaker::bake` signature update.
- `MatterEngine3/tests/script_host_tests.cpp` — emission round-trip test.
- `MatterEngine3/tests/part_graph_integration_tests.cpp` — install-with-placement test.
- `MatterEngine3/tests/viewer_logic_tests.cpp` — compose-expansion test.
- `MatterEngine3/tests/part_asset_v2_tests.cpp` — new-material assertion.

---

## Task 1: Append BARK / LEAF / DIRT materials

**Files:**
- Modify: `MatterSurfaceLib/src/material_registry.c:6-27`
- Test: `MatterEngine3/tests/part_asset_v2_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add this function to `MatterEngine3/tests/part_asset_v2_tests.cpp` (above `main`) and call `test_new_materials();` from `main` alongside the other test calls. It needs the registry header:

```cpp
// at top with the other includes:
#include "material_registry.h"

static void test_new_materials() {
    CHECK(MaterialRegistryCount() == 17, "registry has 17 materials after bark/leaf/dirt");
    const MaterialDef* bark = MaterialRegistryGet(14);
    CHECK(bark->albedo[0] > 0.30f && bark->albedo[2] < 0.20f, "material 14 is brown bark");
    const MaterialDef* leaf = MaterialRegistryGet(15);
    CHECK(leaf->albedo[1] > leaf->albedo[0] && leaf->albedo[1] > leaf->albedo[2],
          "material 15 is green leaf");
    const MaterialDef* dirt = MaterialRegistryGet(16);
    CHECK(dirt->albedo[0] > dirt->albedo[2], "material 16 is brown dirt");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd "MatterEngine3/tests" && make run-partv2`
Expected: FAIL — `registry has 17 materials...` (count is currently 14).

- [ ] **Step 3: Implement — append materials and groups**

In `MatterSurfaceLib/src/material_registry.c`, extend the enum (currently ends `GROUP_SAND = 9`):

```c
enum {
    GROUP_RED = 0, GROUP_BLUE = 1, GROUP_GROUND = 2, GROUP_METAL = 3,
    GROUP_GLASS = 4, GROUP_LIGHT = 5, GROUP_GREENGLASS = 6, GROUP_WATER = 7,
    GROUP_STONE = 8, GROUP_SAND = 9, GROUP_BARK = 10, GROUP_LEAF = 11, GROUP_DIRT = 12
};
```

Append these three rows after index 13 (sand), inside `g_materials[]`:

```c
    /* 14 BARK */ {{0.36f,0.25f,0.16f}, 0.90f, 0.0f, 0.0f, 0.0f, 1.0f, 0, GROUP_BARK, 0},
    /* 15 LEAF */ {{0.22f,0.45f,0.18f}, 0.80f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_LEAF, 0},
    /* 16 DIRT */ {{0.40f,0.28f,0.18f}, 1.00f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_DIRT, 0},
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd "MatterEngine3/tests" && make run-partv2`
Expected: PASS — all checks, including the 4 new material checks.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/material_registry.c MatterEngine3/tests/part_asset_v2_tests.cpp
git commit -m "feat: add bark, leaf, and dirt materials to the registry"
```

---

## Task 2: Fix the `MAT` map and recolor Grass/Terrain

**Files:**
- Modify: `MatterEngine3/src/part_base.js.h:3`
- Modify: `MatterEngine3/examples/world_demo/schemas/Grass.js`
- Modify: `MatterEngine3/examples/world_demo/schemas/Terrain.js`
- Test: `MatterEngine3/tests/example_world.cpp` (existing end-to-end driver)

- [ ] **Step 1: Build the existing end-to-end driver to confirm a clean baseline**

Run: `cd "MatterEngine3/tests" && make run-example`
Expected: PASS / non-error exit (bakes terrain+tree+grass on the current placeholder schemas). Note the output so you can compare after the change.

- [ ] **Step 2: Fix the `MAT` map**

In `MatterEngine3/src/part_base.js.h`, replace line 3:

```js
globalThis.MAT = { stone: 0, dirt: 1, glass: 2 };
```

with:

```js
globalThis.MAT = {
  bark: 14, leaf: 15, dirt: 16,
  grass: 2, stone: 8, stoneDark: 9, rock: 11,
  sand: 13, water: 7, metal: 3, glass: 4, light: 5,
};
```

- [ ] **Step 3: Recolor Grass.js**

In `MatterEngine3/examples/world_demo/schemas/Grass.js`, change every `fill(MAT.glass)` for the blades to `fill(MAT.grass)`. (Leave geometry untouched.)

- [ ] **Step 4: Recolor Terrain.js**

In `MatterEngine3/examples/world_demo/schemas/Terrain.js`, map the dirt slab to `MAT.dirt`, the stone mounds to `MAT.stone` (and any accent mounds to `MAT.rock`). If there is a top layer intended as grass, use `MAT.grass`.

- [ ] **Step 5: Re-bake the example world to verify it still bakes**

Run: `cd "MatterEngine3/tests" && make run-example`
Expected: PASS / non-error exit. The schemas now reference materials 2/8/11/13/16 instead of the placeholder 0/1/2.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/part_base.js.h MatterEngine3/examples/world_demo/schemas/Grass.js MatterEngine3/examples/world_demo/schemas/Terrain.js
git commit -m "fix: map MAT names to correct material ids; recolor grass and terrain"
```

---

## Task 3: `placeChild` accumulator in DslState

**Files:**
- Modify: `MatterEngine3/include/dsl_state.h`
- Modify: `MatterEngine3/src/dsl_state.cpp`
- Modify: `MatterEngine3/src/dsl_bindings.cpp`
- Modify: `MatterEngine3/src/part_base.js.h`

This task adds the data structures + conversion + binding. The end-to-end behavior is tested in Task 5 (it needs the host wiring). This task's verification is a clean compile.

- [ ] **Step 1: Add the placement API to `dsl_state.h`**

Add `#include <map>` at the top. Inside `class DslState`, add a nested struct and public methods (place near the brush-emission section):

```cpp
    // A recorded child-part placement: resolved hash of the child + the world
    // transform (row-major) at the current matrix-stack top when placeChild ran.
    struct ChildPlacement { uint64_t hash; float transform[16]; };

    // Host installs the declared children's module-name -> resolved-hash map
    // before build(); placeChild looks names up here. Empty map => any placeChild
    // is a fail-closed error.
    void set_child_hashes(std::map<std::string, uint64_t> m) { child_hashes_ = std::move(m); }

    // Record a placement of `module` at the current transform-stack top. Unknown
    // module (not in set_child_hashes) -> set_error (fail-closed).
    void placeChild(const std::string& module);

    const std::vector<ChildPlacement>& children() const { return children_; }
```

In the `private:` section add:

```cpp
    std::map<std::string, uint64_t>   child_hashes_;   // declared children (name -> hash)
    std::vector<ChildPlacement>       children_;        // accumulated placements
```

- [ ] **Step 2: Implement `placeChild` + Matrix→row16 in `dsl_state.cpp`**

Add (anywhere in the `dsl` namespace in `dsl_state.cpp`):

```cpp
// raylib Matrix stores its 16 floats column-major (m0,m4,m8,m12 = first row of
// the math matrix). world_flatten / ChildInstance consume row-major, so transpose
// the storage: translation (m12,m13,m14) lands in out[3],out[7],out[11].
static void matrix_to_row16(const Matrix& mm, float out[16]) {
    out[0]=mm.m0;  out[1]=mm.m4;  out[2]=mm.m8;  out[3]=mm.m12;
    out[4]=mm.m1;  out[5]=mm.m5;  out[6]=mm.m9;  out[7]=mm.m13;
    out[8]=mm.m2;  out[9]=mm.m6;  out[10]=mm.m10; out[11]=mm.m14;
    out[12]=mm.m3; out[13]=mm.m7; out[14]=mm.m11; out[15]=mm.m15;
}

void DslState::placeChild(const std::string& module) {
    auto it = child_hashes_.find(module);
    if (it == child_hashes_.end()) {
        set_error("placeChild: undeclared child '" + module +
                  "' (add it to static requires)");
        return;
    }
    ChildPlacement p;
    p.hash = it->second;
    matrix_to_row16(top(), p.transform);
    children_.push_back(p);
}
```

- [ ] **Step 3: Add the binding in `dsl_bindings.cpp`**

Add the C function (next to the other `j_*` functions):

```cpp
static JSValue j_placeChild(JSContext* c, JSValueConst, int, JSValueConst* a){
    const char* m = JS_ToCString(c, a[0]);
    if (m) { state_of(c)->placeChild(m); JS_FreeCString(c, m); }
    return JS_UNDEFINED;
}
```

And register it inside `install_bindings` (next to the other `bind(...)` calls):

```cpp
    bind("__dsl_placeChild", j_placeChild, 1);
```

- [ ] **Step 4: Expose `placeChild` on the JS `Part` class**

In `MatterEngine3/src/part_base.js.h`, add this line inside the `Part` class body (e.g. after `smoothing(k)`):

```js
  placeChild(module)     { __dsl_placeChild(module); }
```

- [ ] **Step 5: Compile to verify it builds**

Run: `cd "MatterEngine3" && make clean && make`
Expected: `libmatter_engine3.a` builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/dsl_state.h MatterEngine3/src/dsl_state.cpp MatterEngine3/src/dsl_bindings.cpp MatterEngine3/src/part_base.js.h
git commit -m "feat: add placeChild DSL binding and child-placement accumulator"
```

---

## Task 4: Thread child module names through `bake_source`

**Files:**
- Modify: `MatterEngine3/include/script_host.h`
- Modify: `MatterEngine3/src/script_host.cpp:498-812`

- [ ] **Step 1: Extend the `bake_source` signature**

In `MatterEngine3/include/script_host.h`, change the `bake_source` declaration to add a parallel `child_modules` pointer:

```cpp
    BakeResult bake_source(const std::string& source,
                           const std::string& params_json,
                           const BakeOptions& opts,
                           const uint64_t* child_hashes = nullptr,
                           size_t child_count = 0,
                           const std::string* child_modules = nullptr);
```

- [ ] **Step 2: Build the name→hash map and write the child table in `script_host.cpp`**

Update the `bake_source` definition signature to match (add `const std::string* child_modules`).

After the `DslState state;` is constructed and before `build()` is invoked, install the declared-children map:

```cpp
    {
        std::map<std::string, uint64_t> name2hash;
        if (child_modules && child_hashes)
            for (size_t i = 0; i < child_count; ++i)
                name2hash[child_modules[i]] = child_hashes[i];
        state.set_child_hashes(std::move(name2hash));
    }
```

Then, in the success path where `save_v2` is currently called with `nullptr, 0` (around line 808), convert the accumulated placements and pass them:

```cpp
        std::vector<part_asset::ChildInstance> kids;
        kids.reserve(state.children().size());
        for (const auto& c : state.children()) {
            part_asset::ChildInstance ci;
            ci.child_resolved_hash = c.hash;
            std::memcpy(ci.transform, c.transform, sizeof ci.transform);
            kids.push_back(ci);
        }
        std::string path = part_asset::cache_path_resolved(r.resolved_hash);
        part_asset::LodLevels lods{};
        bool ok = part_asset::save_v2(path, blas, tlas,
                                      kids.empty() ? nullptr : kids.data(), kids.size(),
                                      lods, r.resolved_hash);
```

Ensure `<map>` and `<cstring>` are included in `script_host.cpp` (add if missing).

- [ ] **Step 3: Compile to verify it builds**

Run: `cd "MatterEngine3" && make clean && make`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/include/script_host.h MatterEngine3/src/script_host.cpp
git commit -m "feat: bake_source records placed child instances into the .part"
```

---

## Task 5: Emission round-trip test

**Files:**
- Test: `MatterEngine3/tests/script_host_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add this function to `MatterEngine3/tests/script_host_tests.cpp` and call `test_place_child_roundtrip();` from `main`. It bakes a leaf, then a parent that places it twice, then reloads the parent's `.part` and asserts two child rows with correct hashes and transforms. (Match the include/style of the existing tests in that file; `part_asset_v2.h` is already linked.)

```cpp
static void test_place_child_roundtrip() {
    using namespace script_host;
    ScriptHost host;

    const char* leaf_src =
        "export default class Leaf extends Part {"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.leaf);"
        "            this.sphere([0,0,0],0.1); this.endVoxels(); } }";
    BakeResult lr = host.bake_source(leaf_src, "{}", {});
    CHECK(lr.error.ok, "leaf bakes");
    uint64_t leaf_hash = lr.resolved_hash;

    const char* parent_src =
        "export default class P extends Part {"
        "  build(p){"
        "    this.beginVoxels(0.2); this.fill(MAT.bark);"
        "    this.box([0,0,0],[0.2,0.2,0.2]); this.endVoxels();"
        "    this.pushMatrix(); this.translate(2,3,4); this.placeChild('Leaf'); this.popMatrix();"
        "    this.pushMatrix(); this.translate(5,0,0); this.placeChild('Leaf'); this.popMatrix();"
        "  } }";
    uint64_t kids[1]   = { leaf_hash };
    std::string names[1] = { std::string("Leaf") };
    BakeResult pr = host.bake_source(parent_src, "{}", {}, kids, 1, names);
    CHECK(pr.error.ok, "parent bakes with placed children");

    BLASManager blas; TLASManager tlas;
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::string ppath = part_asset::cache_path_resolved(pr.resolved_hash);
    bool loaded = part_asset::load_v2(ppath, pr.resolved_hash, blas, tlas, children, lods);
    CHECK(loaded, "parent .part reloads");
    CHECK(children.size() == 2, "two leaf instances recorded");
    if (children.size() == 2) {
        CHECK(children[0].child_resolved_hash == leaf_hash, "child 0 is the leaf");
        CHECK(children[1].child_resolved_hash == leaf_hash, "child 1 is the leaf");
        // row-major translation lives in transform[3],[7],[11]
        CHECK(children[0].transform[3]  == 2.0f &&
              children[0].transform[7]  == 3.0f &&
              children[0].transform[11] == 4.0f, "child 0 placed at (2,3,4)");
        CHECK(children[1].transform[3]  == 5.0f, "child 1 placed at x=5");
    }
}
```

Add includes at the top if not present:

```cpp
#include "../include/part_asset_v2.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
```

- [ ] **Step 2: Run test to verify it fails (then passes)**

Run: `cd "MatterEngine3/tests" && make run-script`
Expected: PASS — all checks. (If you run this before Task 3/4 are merged it FAILS to compile / `children.size()==0`; with them merged it passes.)

- [ ] **Step 3: Determinism check (re-bake stability)**

Add to the same test, after the parent bake, a second bake and assert the hash is stable:

```cpp
    BakeResult pr2 = host.bake_source(parent_src, "{}", {}, kids, 1, names);
    CHECK(pr2.error.ok && pr2.resolved_hash == pr.resolved_hash,
          "parent re-bake is deterministic");
```

Run: `cd "MatterEngine3/tests" && make run-script`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/tests/script_host_tests.cpp
git commit -m "test: verify placeChild round-trips through save_v2/load_v2"
```

---

## Task 6: Thread child names through the PartGraph Baker

**Files:**
- Modify: `MatterEngine3/include/part_graph.h:49-63,124-135`
- Modify: `MatterEngine3/src/part_graph.cpp:42-168,271-279`
- Modify: `MatterEngine3/tests/part_graph_tests.cpp:42-66`

- [ ] **Step 1: Update the `Baker::bake` interface in `part_graph.h`**

Change the abstract `Baker::bake` declaration to add `child_modules`:

```cpp
    virtual bool bake(const std::string& source, const Params& params,
                      const std::vector<uint64_t>& child_hashes,
                      const std::vector<std::string>& child_modules,
                      uint64_t resolved_hash) = 0;
```

And the `HostBaker::bake` override declaration (in the `MATTER_HAVE_SCRIPT_HOST` block):

```cpp
    bool bake(const std::string& source, const Params& params,
              const std::vector<uint64_t>& child_hashes,
              const std::vector<std::string>& child_modules,
              uint64_t resolved_hash) override;
```

- [ ] **Step 2: Carry child module names in `InternalNode` and pass them to bake**

In `part_graph.cpp`, add to `struct InternalNode`:

```cpp
    std::vector<std::string> child_modules;   // direct children's module names (parallel to child_hashes)
```

In the `resolve` lambda's child loop, collect names parallel to hashes:

```cpp
            std::vector<uint64_t> child_keys, child_hashes;
            std::vector<std::string> child_modules;
            for (const auto& kid : kids) {
                uint64_t ck = 0;
                if (!resolve(kid, ck)) { stack.pop_back(); on_stack.erase(key); return false; }
                child_keys.push_back(ck);
                child_hashes.push_back(memo.at(ck).resolved_hash);
                child_modules.push_back(kid.module);
            }
```

Store it on the node: `node.child_modules = child_modules;`

In the topo bake loop, pass it:

```cpp
        if (!baker_.bake(n.source, n.params, n.child_hashes, n.child_modules, n.resolved_hash)) {
```

- [ ] **Step 3: Forward names in `HostBaker::bake`**

In the `MATTER_HAVE_SCRIPT_HOST` block of `part_graph.cpp`, update `HostBaker::bake`:

```cpp
bool HostBaker::bake(const std::string& source, const Params& params,
                     const std::vector<uint64_t>& child_hashes,
                     const std::vector<std::string>& child_modules,
                     uint64_t resolved_hash) {
    script_host::BakeResult r = host_.bake_source(
        source, params_to_json(params), /*opts*/{},
        child_hashes.data(), child_hashes.size(),
        child_modules.data());
    return r.error.ok && r.resolved_hash == resolved_hash;
}
```

- [ ] **Step 4: Update `FakeBaker` in the unit test**

In `MatterEngine3/tests/part_graph_tests.cpp`, change the `FakeBaker::bake` override to match the new signature (it can ignore the new arg):

```cpp
    bool bake(const std::string&, const Params&,
              const std::vector<uint64_t>& child_hashes,
              const std::vector<std::string>& /*child_modules*/, uint64_t h) override {
        if (fail_hashes.count(h)) return false;
        bake_order.push_back(h);
        children_seen[h] = child_hashes;
        on_disk.insert(h);
        return true;
    }
```

- [ ] **Step 5: Run the graph unit tests**

Run: `cd "MatterEngine3/tests" && make run-graph`
Expected: PASS — existing topo/cache/cycle checks unaffected.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/part_graph.h MatterEngine3/src/part_graph.cpp MatterEngine3/tests/part_graph_tests.cpp
git commit -m "feat: thread child module names through the PartGraph baker"
```

---

## Task 7: Install-with-placement integration test

**Files:**
- Modify: `MatterEngine3/tests/part_graph_integration_tests.cpp`

- [ ] **Step 1: Write the failing test**

This test drives the REAL `FileModuleResolver`/`HostBaker` against two on-disk schemas: a leaf and a parent that `static requires` the leaf and places it in `build()`. Add a temp schema dir + the test. Follow the existing file's helpers for the schemas dir (it already constructs a `FileModuleResolver(host, schemas_dir)` and `HostBaker`). Write the two `.js` files into a temp dir, install the parent, then load the parent `.part` and assert child rows.

```cpp
static void test_install_with_placement() {
    namespace pg = part_graph;
    // temp schema dir
    std::string dir = "tmp_place_schemas";
    fs_mkdir_p(dir);   // use the file's existing dir helper, or std::filesystem::create_directories
    write_file(dir + "/LeafX.js",
        "export default class LeafX extends Part {"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.leaf);"
        "            this.sphere([0,0,0],0.1); this.endVoxels(); } }");
    write_file(dir + "/TreeX.js",
        "export default class TreeX extends Part {"
        "  static requires = [{ module: 'LeafX' }];"
        "  build(p){"
        "    this.beginVoxels(0.2); this.fill(MAT.bark);"
        "    this.box([0,0,0],[0.3,0.3,0.3]); this.endVoxels();"
        "    this.pushMatrix(); this.translate(0,1,0); this.placeChild('LeafX'); this.popMatrix();"
        "  } }");

    script_host::ScriptHost host;
    pg::FileModuleResolver resolver(host, dir);
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    std::vector<pg::ChildRequest> roots = { { "TreeX", {} } };
    pg::InstallResult ir = graph.install(roots);
    CHECK(ir.ok, "install of TreeX with a required+placed LeafX succeeds");

    // resolve the TreeX hash the same way the host does, then load its .part
    uint64_t leaf_hash = host.resolve_hash(read_file(dir + "/LeafX.js"), "{}");
    uint64_t kids[1] = { leaf_hash };
    uint64_t tree_hash = host.resolve_hash(read_file(dir + "/TreeX.js"), "{}", kids, 1);

    BLASManager blas; TLASManager tlas;
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    bool loaded = part_asset::load_v2(part_asset::cache_path_resolved(tree_hash),
                                      tree_hash, blas, tlas, children, lods);
    CHECK(loaded, "TreeX .part reloads");
    CHECK(children.size() == 1, "TreeX recorded one LeafX instance");
    if (children.size() == 1) {
        CHECK(children[0].child_resolved_hash == leaf_hash, "instance points at LeafX");
        CHECK(children[0].transform[7] == 1.0f, "instance placed at y=1");
    }
}
```

Use this file's existing file-IO helpers if present; otherwise add small `write_file`/`read_file` helpers using `<fstream>` and `std::filesystem::create_directories` for `fs_mkdir_p`. Call `test_install_with_placement();` from `main`.

- [ ] **Step 2: Run the integration test**

Run: `cd "MatterEngine3/tests" && make run-graph-integration`
Expected: PASS — install succeeds and the TreeX `.part` carries exactly one LeafX child at y=1.

- [ ] **Step 3: Commit**

```bash
git add MatterEngine3/tests/part_graph_integration_tests.cpp
git commit -m "test: install a part that requires and places a child leaf"
```

---

## Task 8: Author the Leaf part

**Files:**
- Create: `MatterEngine3/examples/world_demo/schemas/Leaf.js`

- [ ] **Step 1: Create `Leaf.js`**

```js
// A single small flattened leaf blob in leaf-green. Pure part (no children).
export default class Leaf extends Part {
  build(p) {
    this.beginVoxels(0.06);
    this.fill(MAT.leaf);
    this.pushMatrix();
    this.scale(1.0, 0.35, 1.0);   // flatten on Y so it reads as a leaf
    this.sphere([0, 0, 0], 0.12);
    this.popMatrix();
    this.endVoxels();
  }
}
```

- [ ] **Step 2: Verify the leaf bakes via the example driver**

The example driver bakes whatever the manifest references; to bake the leaf directly, temporarily confirm through the integration path is already covered. Here just sanity-check JS validity by baking it through the script host test harness already present, or rely on Task 9's Tree (which requires Leaf) to bake it. Minimal check now:

Run: `cd "MatterEngine3/tests" && make run-example`
Expected: still passes (Leaf is not yet referenced by the manifest; this confirms no syntax regression in the shared `part_base.js`).

- [ ] **Step 3: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Leaf.js
git commit -m "feat: add the Leaf instanced part"
```

---

## Task 9: Author the L-system Tree

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Tree.js` (full rewrite)
- Test: `MatterEngine3/tests/part_graph_integration_tests.cpp`

- [ ] **Step 1: Rewrite `Tree.js`**

A broadleaf tree: an L-system string drives a turtle that lays tapered bark spheres along each segment (detailed trunk + branches) and places a `Leaf` child at each terminal twig. Deterministic via the seeded `Math.random`.

```js
import { expand } from 'shared-lib/lsystem';

export default class Tree extends Part {
  static requires = [{ module: 'Leaf' }];

  build(p) {
    const MAX_LEAVES = 400;
    let leaves = 0;

    // L-system: F = grow a segment, [ ] = branch push/pop, +-&^ = turns.
    const rules = {
      F: [
        { to: 'FF[+F&F][-F^F][&F+F]', weight: 3 },
        { to: 'FF[-F^F][+F&F]',       weight: 2 },
      ],
    };
    const seed = 1337;
    const sys = expand('F', rules, 4, seed);

    // Turtle: walk the string, maintaining the matrix stack + a depth-driven
    // taper. Each 'F' lays several bark spheres along a unit-ish step.
    const STEP = 0.55;
    const TURN = 22 * Math.PI / 180;
    let depth = 0;

    this.beginVoxels(0.12);

    const branchRadius = (d) => Math.max(0.06, 0.42 * Math.pow(0.78, d));

    const segment = (d) => {
      this.fill(MAT.bark);
      const r0 = branchRadius(d);
      const r1 = branchRadius(d + 1);
      const N = 4;
      for (let i = 0; i <= N; ++i) {
        const t = i / N;
        const y = STEP * t;
        const r = r0 + (r1 - r0) * t;
        this.pushMatrix();
        this.translate(0, y, 0);
        this.sphere([0, 0, 0], r);
        this.popMatrix();
      }
      this.translate(0, STEP, 0);   // advance the turtle to the segment end
    };

    const placeLeaf = () => {
      if (leaves >= MAX_LEAVES) return;
      this.pushMatrix();
      // small seeded orient jitter so foliage isn't uniform
      this.rotateY(Math.random() * Math.PI * 2);
      this.rotateX((Math.random() - 0.5) * 0.8);
      this.translate(0, 0.15, 0);
      this.placeChild('Leaf');
      this.popMatrix();
      ++leaves;
    };

    for (let i = 0; i < sys.length; ++i) {
      const ch = sys[i];
      if (ch === 'F') {
        segment(depth);
        // a terminal twig: next char ends this branch
        const next = sys[i + 1];
        if (next === ']' || next === undefined) placeLeaf();
      } else if (ch === '[') {
        this.pushMatrix(); ++depth;
      } else if (ch === ']') {
        this.popMatrix(); --depth;
      } else if (ch === '+') {
        this.rotateZ(TURN);
      } else if (ch === '-') {
        this.rotateZ(-TURN);
      } else if (ch === '&') {
        this.rotateX(TURN);
      } else if (ch === '^') {
        this.rotateX(-TURN);
      }
    }

    this.endVoxels();
  }
}
```

- [ ] **Step 2: Add a Tree-bakes-with-leaves test**

In `MatterEngine3/tests/part_graph_integration_tests.cpp`, add a test that installs the REAL demo schemas dir (`../examples/world_demo/schemas`) for root `Tree`, then loads the Tree `.part` and asserts it carries leaf children. Compute the Tree hash via `host.resolve_hash(treeSrc, "{}", &leafHash, 1)`.

```cpp
static void test_demo_tree_has_leaves() {
    namespace pg = part_graph;
    const std::string schemas = "../examples/world_demo/schemas";
    script_host::ScriptHost host;
    host.set_shared_lib_root("../shared-lib");
    pg::FileModuleResolver resolver(host, schemas);
    pg::HostBaker baker(host, ".");
    pg::PartGraph graph(resolver, baker);

    std::vector<pg::ChildRequest> roots = { { "Tree", {} } };
    CHECK(graph.install(roots).ok, "demo Tree installs");

    uint64_t leaf_hash = host.resolve_hash(read_file(schemas + "/Leaf.js"), "{}");
    uint64_t kids[1] = { leaf_hash };
    uint64_t tree_hash = host.resolve_hash(read_file(schemas + "/Tree.js"), "{}", kids, 1);

    BLASManager blas; TLASManager tlas;
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    bool loaded = part_asset::load_v2(part_asset::cache_path_resolved(tree_hash),
                                      tree_hash, blas, tlas, children, lods);
    CHECK(loaded, "demo Tree .part reloads");
    CHECK(!children.empty(), "demo Tree placed at least one leaf");
    for (const auto& c : children)
        CHECK(c.child_resolved_hash == leaf_hash, "every Tree child is a Leaf");
}
```

Call `test_demo_tree_has_leaves();` from `main`. (`set_shared_lib_root` is needed because `Tree.js` imports `shared-lib/lsystem`.)

- [ ] **Step 3: Run the integration test**

Run: `cd "MatterEngine3/tests" && make run-graph-integration`
Expected: PASS — the demo Tree bakes, and its `.part` carries one-or-more Leaf instances, all pointing at the Leaf hash.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/Tree.js MatterEngine3/tests/part_graph_integration_tests.cpp
git commit -m "feat: L-system broadleaf tree that places instanced Leaf children"
```

---

## Task 10: PartStore keeps the child table

**Files:**
- Modify: `MatterEngine3/viewer/part_store.h:18-22`
- Modify: `MatterEngine3/viewer/part_store.cpp`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

- [ ] **Step 1: Add `children` to `LoadedPart`**

In `part_store.h`, include the v2 header and add the field:

```cpp
#include "part_asset_v2.h"   // part_asset::ChildInstance
```

```cpp
struct LoadedPart {
    std::vector<BLASHandle> lod_blas;
    float                   bound_radius = 0.0f;
    std::vector<float>      thresholds;
    std::vector<part_asset::ChildInstance> children;   // baked child-instance table (may be empty)
};
```

- [ ] **Step 2: Populate `children` in `get_or_load`**

In `part_store.cpp`, the `get_or_load` body already calls `load_v2(... children_out ...)`. Store the returned children into the `LoadedPart` before returning it (find the `load_v2` call and assign its `children_out` into the part being inserted into `loaded_`). Concretely, after a successful `load_v2`, set `lp.children = std::move(children_out);` on the `LoadedPart` you build.

- [ ] **Step 3: Write the failing test**

In `MatterEngine3/tests/viewer_logic_tests.cpp`, add a test that bakes a parent-with-child into a temp cache (reuse the install path used elsewhere in this file, or call the script host directly), then `get_or_load`s the parent and asserts `children` is non-empty. If the file already bakes a demo world, load the Tree hash and assert `get_or_load(tree)->children` is non-empty.

```cpp
static void test_partstore_keeps_children() {
    // Assumes the demo world has been installed into cache_root by the harness
    // (as other viewer_logic tests do). Resolve the Tree hash and check children.
    viewer::PartStore store(/*cache_root*/ "cache");
    // <hash resolution mirrors test_demo_tree_has_leaves in the integration test;
    //  reuse the same resolve_hash calls or a known fixture hash the harness baked.>
    const viewer::LoadedPart* lp = store.get_or_load(/*tree_hash*/ g_demo_tree_hash);
    CHECK(lp != nullptr, "tree part loads");
    CHECK(lp && !lp->children.empty(), "loaded tree part carries its child table");
}
```

If `viewer_logic_tests.cpp` has no existing demo-install fixture, install one in the test using `part_graph` (mirror Task 9's setup) and capture `g_demo_tree_hash`. Call the test from `main`.

- [ ] **Step 4: Run the viewer logic test**

Run: `cd "MatterEngine3/tests" && make run-viewer-logic`
Expected: PASS — `get_or_load` returns a part whose `children` table is non-empty.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/viewer/part_store.h MatterEngine3/viewer/part_store.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: PartStore retains the baked child-instance table"
```

---

## Task 11: WorldComposer emits own-geometry plus placed children

**Files:**
- Modify: `MatterEngine3/viewer/world_composer.cpp`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

- [ ] **Step 1: Write the failing test**

Add a test asserting that composing a world containing a tree records MORE TLAS instances than there are root instances (because each tree expands into trunk + N leaves). With one Tree root that has K leaf children, the recorded count must be `1 + K` for that root.

```cpp
static void test_compose_expands_children() {
    // Build a minimal WorldState with exactly one Tree instance at identity,
    // using the same store/harness the other viewer_logic tests use.
    // (Mirror the existing compose test's setup for WorldState + resolver.)
    viewer::PartStore store("cache");
    const viewer::LoadedPart* tree = store.get_or_load(g_demo_tree_hash);
    CHECK(tree && !tree->children.empty(), "precondition: tree has children");
    size_t expected = 1 + tree->children.size();   // trunk + leaves (leaves are pure parts)

    viewer::WorldComposer composer(store, /*tlas_capacity*/ expected + 16);
    // <compose a WorldState holding one Tree at identity with a PassThroughResolver;
    //  reuse the existing compose test's WorldState construction>
    int recorded = composer.compose(world_state_one_tree, pass_resolver, lods, cam0);
    CHECK((size_t)recorded == expected, "one tree expands into trunk + its leaves");
}
```

(Reuse the exact `WorldState`/resolver/`lods`/`cam` construction already present in this file's existing `WorldComposer` test; only the assertion on the expanded count is new.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd "MatterEngine3/tests" && make run-viewer-logic`
Expected: FAIL — `recorded` is currently `1` (root only), not `1 + K`.

- [ ] **Step 3: Implement recursive expansion in `world_composer.cpp`**

Replace the per-root single-record loop with a recursive emit that records the node's own BLAS at its world transform, then recurses into its children at `world × child.transform`. Children render at LOD0.

```cpp
#include "world_composer.h"
#include <cstring>

namespace viewer {

// Row-major 4x4 multiply, matching world_flatten's convention.
static void mul16(const float* a, const float* b, float* out) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

int WorldComposer::compose(const WorldState& state,
                           SectorResolver& resolver,
                           const lod_select::PartLodTable& lods,
                           const float3& cam_pos) {
    auto resolved = resolver.resolve(state, lods, cam_pos);

    std::vector<TLASManager::DrawInstance> insts;

    const int kMaxDepth = 8;
    const size_t kMaxInstances = 200000;

    // Recursive emit: own BLAS at `world`, then children at world*child.
    std::function<void(uint64_t, const float*, int, int)> emit =
        [&](uint64_t hash, const float* world, int lod, int depth) {
            if (depth > kMaxDepth || insts.size() >= kMaxInstances) return;
            const LoadedPart* lp = store_.get_or_load(hash);
            if (!lp || lp->lod_blas.empty()) return;
            int use_lod = lod;
            if (use_lod < 0) use_lod = 0;
            if (use_lod >= (int)lp->lod_blas.size()) use_lod = (int)lp->lod_blas.size() - 1;

            TLASManager::DrawInstance di;
            di.blas_handle = lp->lod_blas[use_lod];
            di.material_id = 0;
            di.is_imposter = false;
            std::memcpy(di.transform.m, world, sizeof(di.transform.m));
            insts.push_back(di);

            for (const auto& c : lp->children) {
                float child_world[16];
                mul16(world, c.transform, child_world);
                emit(c.child_resolved_hash, child_world, 0, depth + 1);
            }
        };

    for (const auto& r : resolved) {
        int lod = r.lod_level;
        emit(r.part_hash, r.transform, lod, 0);
    }

    tlas_.clear();
    tlas_.draw_batch(insts);
    tlas_.build(store_.blas());
    return (int)insts.size();
}

} // namespace viewer
```

Add `#include <functional>` at the top. Confirm `r.transform` is a `float[16]` (the prior code `memcpy`d it into `di.transform.m`); if it is a fixed array member, pass `r.transform` directly as shown.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd "MatterEngine3/tests" && make run-viewer-logic`
Expected: PASS — one tree expands into `1 + K` recorded instances.

- [ ] **Step 5: Run the whole headless suite for regressions**

Run: `cd "MatterEngine3" && make test && (cd tests && make run-graph && make run-graph-integration && make run-comp && make run-viewer-logic)`
Expected: all suites PASS. Root-only parts (Terrain, Grass) still record exactly one instance each.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/viewer/world_composer.cpp MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: compose expands placed child instances into the TLAS"
```

---

## Task 12: Build the viewer and capture a verification screenshot

**Files:**
- (No source changes — build + visual verification)

- [ ] **Step 1: Build the Linux viewer**

Run: `cd "MatterEngine3/viewer" && make clean && make`
Expected: `viewer` binary builds.

- [ ] **Step 2: Delete the stale cache so parts re-bake with new materials/children**

Run: `cd "MatterEngine3/viewer" && rm -rf cache/parts`
Expected: cache cleared (forces a fresh install/bake on next run).

- [ ] **Step 3: Headless screenshot**

Run: `cd "MatterEngine3/viewer" && DISPLAY=:0 MATTER_SCREENSHOT=world.png ./viewer`
Expected: prints `screenshot written to world.png` and exits 0. (Software GL warm-up ~60s — be patient.)

- [ ] **Step 4: Inspect the screenshot**

Read `MatterEngine3/viewer/world.png`. Confirm: brown trunks with branching, green scattered leaves (not glass spheres), non-glass grass, brown/stone terrain. If foliage is missing, check that `cache/parts` was cleared and that the Tree `.part` carries children (Task 9 test).

- [ ] **Step 5: Commit the build artifacts only if the project tracks them; otherwise stop here**

No commit unless the repo intentionally tracks built binaries (it does not by default). This task is verification only.

---

## Task 13: Cross-compile the Windows viewer

**Files:**
- (No source changes — Windows build for the user to test)

- [ ] **Step 1: Build viewer.exe + shaders**

Run: `cd "MatterEngine3/viewer" && make windows && make win-shaders`
Expected: `viewer.exe` (PE32+ x86-64) and the `shaders/` dir populated, matching the prior known-good build.

- [ ] **Step 2: Report to the user**

Tell the user `viewer.exe` is ready under `MatterEngine3/viewer/` for them to launch (per memory: GUI apps are launched by the user, not the harness).

---

## Self-Review

**1. Spec coverage**
- Spec §3 Materials → Tasks 1 (registry) + 2 (MAT map, recolor). ✓
- Spec §4 Emission bridge → Task 3 (placeChild/accumulator/conversion), Task 4 (bake_source map + save_v2 table), Task 6 (PartGraph name threading), tested by Tasks 5 + 7. ✓
- Spec §5 Tree+Leaf → Tasks 8 (Leaf) + 9 (Tree). ✓
- Spec §6 Viewer expansion → Tasks 10 (PartStore.children) + 11 (recursive emit, own-geometry-at-every-node). ✓
- Spec §8 Testing → unit/integration tests in Tasks 1,5,7,9,10,11; screenshot in Task 12; regression in Task 11 step 5. ✓

**2. Placeholder scan** — Tests in Tasks 10 and 11 reference the existing `viewer_logic_tests.cpp` WorldState/resolver fixtures and a `g_demo_tree_hash` the implementer must wire from that file's existing harness; this is intentional reuse, not a code placeholder. All engine-code steps contain complete code. No TBD/TODO.

**3. Type consistency** — `placeChild(const std::string&)`, `set_child_hashes(std::map<std::string,uint64_t>)`, `children()`/`ChildPlacement{hash,transform[16]}`, `Baker::bake(..., const std::vector<std::string>& child_modules, ...)`, `bake_source(..., const std::string* child_modules)`, `LoadedPart.children` (`std::vector<part_asset::ChildInstance>`), and row-major translation slots `[3]/[7]/[11]` are used consistently across Tasks 3–11.
