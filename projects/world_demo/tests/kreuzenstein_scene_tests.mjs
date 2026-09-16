// Exercise the authored scene against the actual Part prelude, with native calls recorded.
// This catches obsolete DSL calls and mismatched child/material params before a GPU bake.
import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";
import path from "node:path";
import { fileURLToPath } from "node:url";
const root = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../../..",
);
const scene = path.join(root, "projects/world_demo/scenes/Kreuzenstein");
// Object lookup mirrors the engine's search path: a scene owns its own
// objects/ over the shared project tier. KreuzensteinBrick lives in the
// project tier because shared-lib/kreuzenstein.js places it by name, and a
// shared-lib module is reachable from every scene (world_definition_tests
// rejects shared-lib references to scene-local objects).
const objectDirs = [
  path.join(scene, "objects"),
  path.join(root, "projects/world_demo/objects"),
];
function readObject(module) {
  for (const dir of objectDirs) {
    const file = path.join(dir, module + ".js");
    if (fs.existsSync(file)) return fs.readFileSync(file, "utf8");
  }
  throw new Error(`${module}.js not found under ${objectDirs.join(", ")}`);
}
const prelude = fs
  .readFileSync(path.join(root, "MatterEngine3/src/part_base.js.h"), "utf8")
  .split('R"JS(')[1]
  .split(')JS"')[0];
const helper = fs
  .readFileSync(
    path.join(root, "projects/world_demo/shared-lib/kreuzenstein.js"),
    "utf8",
  )
  .replaceAll("export ", "");
const world = vm.createContext({
  World: class {},
  defineMaterial: (_name, spec) => {
    assert.equal(spec.metallic, 0);
    return nextMaterial++;
  },
});
let nextMaterial = 30;
vm.runInContext(
  fs.readFileSync(path.join(scene, "Kreuzenstein.js"), "utf8") +
    "\nglobalThis.roots=Kreuzenstein.roots;",
  world,
);
assert.equal(nextMaterial, 34);
const canonical = (o) =>
  JSON.stringify(
    Object.fromEntries(
      Object.entries(o).sort(([a], [b]) => a.localeCompare(b)),
    ),
  );
let totalBricks = 0,
  totalVertices = 0;
function evaluate(module, params) {
  const counts = { bricks: 0, vertices: 0, solids: 0, voxels: 0 };
  let matrixDepth = 0,
    shapeDepth = 0,
    voxelDepth = 0;
  let declared = [];
  let material;
  const context = vm.createContext({ Math });
  for (const name of new Set(prelude.match(/__dsl_\w+/g))) {
    context[name] = (...args) => {
      for (const a of args.flat(Infinity))
        if (typeof a === "number")
          assert.ok(Number.isFinite(a), `${module}: nonfinite ${name}`);
      if (name === "__dsl_pushMatrix") matrixDepth++;
      if (name === "__dsl_popMatrix") assert.ok(--matrixDepth >= 0);
      if (name === "__dsl_beginShape") shapeDepth++;
      if (name === "__dsl_endShape") assert.ok(--shapeDepth >= 0);
      if (name === "__dsl_beginVoxels") {
        voxelDepth++;
        counts.voxels++;
      }
      if (name === "__dsl_endVoxels") assert.ok(--voxelDepth >= 0);
      if (name === "__dsl_vertex") counts.vertices++;
      if (
        ["__dsl_box", "__dsl_cylinder", "__dsl_sphere", "__dsl_cone"].includes(
          name,
        )
      )
        counts.solids++;
      if (name === "__dsl_fill") material = args[0];
      if (name === "__dsl_placeChild") {
        assert.equal(args[0], "KreuzensteinBrick");
        assert.ok(
          args[1].material >= 30 && args[1].material <= 33,
          "voxel brick must use an authored limestone material",
        );
        assert.ok(
          declared.some(
            (d) =>
              d.module === args[0] &&
              canonical(d.params) === canonical(args[1]),
          ),
          "child parameters must match a declared variant",
        );
        counts.bricks++;
      }
    };
  }
  vm.runInContext(prelude, context);
  const source = readObject(module).replace(
    /^import .*;$/m,
    "const Geo=CastleGeometry;",
  );
  vm.runInContext(
    helper + "\n" + source + "\nglobalThis.Ctor=" + module + ";",
    context,
  );
  const p = { ...context.Ctor.params, ...params };
  for (const value of Object.values(p))
    assert.ok(
      ["number", "string", "boolean"].includes(typeof value),
      "part parameters must be flat scalar values",
    );
  declared = context.Ctor.requires?.(p) || [];
  new context.Ctor().build(p);
  assert.equal(matrixDepth, 0);
  assert.equal(shapeDepth, 0);
  assert.equal(voxelDepth, 0);
  if (p.stage === "masonry") {
    assert.equal(
      counts.vertices + counts.solids,
      0,
      "masonry is assembled entirely from voxel children",
    );
    assert.ok(counts.bricks > 0);
  }
  if (p.stage === "details") assert.equal(counts.bricks, 0);
  if (module === "KreuzensteinBrick") {
    assert.equal(counts.voxels, 1);
    assert.equal(material, p.material);
  }
  totalBricks += counts.bricks;
  totalVertices += counts.vertices;
  return counts;
}
for (const r of world.roots) {
  if (r.transform) {
    assert.equal(r.transform.length, 16);
    assert.ok(r.transform.every(Number.isFinite));
  }
  evaluate(r.module, r.params);
}
for (let seed = 0; seed < 4; seed++)
  evaluate("KreuzensteinBrick", { seed, material: 30 + seed });
assert.ok(
  totalBricks > 20000,
  "castle must retain its individual masonry assembly",
);
console.log(
  `Kreuzenstein scene: PASS — ${world.roots.length} roots, ${totalBricks} voxel bricks, ${totalVertices / 3} explicit detail triangles, 4 voxel-CSG material variants.`,
);
